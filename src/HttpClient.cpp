#include "HttpClient.h"

#include <Windows.h>
#include <Winhttp.h>

#pragma comment(lib, "Winhttp.lib")

#include <map>
#include <list>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cwchar>

#include "StringConvertor.h"

#define ERROR_HTTP_HEADER_NOT_FOUND 12150

SimpleStringInputStream::SimpleStringInputStream(const String& str)
: m_buffer()
, m_offset(0)
{
    StringConvertor::FromString(str, m_buffer);
}

SimpleStringInputStream::SimpleStringInputStream(const std::string& str)
: m_buffer(str)
, m_offset(0)
{
}

int64_t SimpleStringInputStream::GetAvailCount() const
{
    if (m_buffer.length() < m_offset)
        return 0;

    return m_buffer.length() - m_offset;
}

uint32_t SimpleStringInputStream::Read(uint8_t *buffer, uint32_t read)
{
    uint32_t readCount = static_cast<uint32_t>(min(GetAvailCount(), static_cast<int64_t>(read)));
    ::memcpy(buffer, m_buffer.data() + m_offset, readCount);

    return readCount;
}

int64_t SimpleStringInputStream::GetTotal() const
{
    return m_buffer.length();
}

class DefaultRedirectCompletionGenericDelegate : public RedirectCompletionGenericDelegate
{
public:
    DefaultRedirectCompletionGenericDelegate()
        : RedirectCompletionGenericDelegate(NULL, NULL)
    {}

public:
    virtual void OnRequestDataFilled() {}
    virtual void OnHeaderAvailable(const Net::HttpResponseHeaders& headers) {}
    virtual void OnBodyAvailable(InputStream& inputStream){}
    virtual void OnCompleted() {}
    virtual void OnError(Exception *ex)
    {
        delete ex;
    }

public:
    virtual void SetLocation(const Net::URL& url) {}
    virtual bool PerformRedirecting() const { return true; }
};

RedirectCompletionGenericDelegate *RedirectCompletionGenericDelegate::GetDefaultDelegate()
{
    static DefaultRedirectCompletionGenericDelegate delegate;
    return &delegate;
}

namespace Net
{
    namespace Details
    {
        class AbstractHttpHandler;
        typedef std::list<AbstractHttpHandler *> HttpHandlers;
        typedef std::map<String, HINTERNET> HostConnections;
    }

    class HttpSession::Private
    {
    protected:
        struct HttpSecurityOptions
        {
            bool isHttps;
            //! cert
        };

    protected:
        HINTERNET m_hSession;
        Details::HostConnections m_connections;
        Details::HttpHandlers m_handlers;

        HttpSessionConfig m_config;

    public:
        Private()
            : m_hSession(NULL)
            , m_connections()
            , m_handlers()
            , m_config()
        {}

        Private(const HttpSessionConfig& config)
            : m_hSession(NULL)
            , m_connections()
            , m_handlers()
            , m_config(config)
        {}

        virtual ~Private()
        {
        }

    public:
        HINTERNET AcquireRequest(const URL& url, const HttpVerb& verb);
        void Disconnect();

    public:
        //! send request
        //! ###
        //! When disconnecting, this method should never be invoked
        //!
        virtual void SendRequest(HINTERNET hReq, const HttpRequest *req, AsyncCompletionGenericDelegate *delegate);
        //! notify handlers
        virtual void OnDisconnect();
        virtual void OnHandleFinished(Details::AbstractHttpHandler *handler);

        //! send redirect request
        virtual void SendRedirect(const HttpRequest *req,
            AsyncCompletionGenericDelegate *delegate,
            RedirectCompletionGenericDelegate *redirectDelegate,
            const HttpResponseHeaders& headers);

    private:
        HINTERNET AcquireConnection(const String& host, uint16_t port);
        HINTERNET OpenRequest(HINTERNET connection, const String& path, HttpVerb verb, const HttpSecurityOptions& securityOpts);
    };

    class LockHttpSessionPrivate : public HttpSession::Private
    {
    private:
        CriticalSection m_lock;
        //! when terminating, the state of the event will shift to unsignaled
        ManualResetEvent m_disconnectedEvent;

    public:
        LockHttpSessionPrivate();
        explicit LockHttpSessionPrivate(const HttpSessionConfig& config);

    public:
        virtual void SendRequest(HINTERNET hReq, const HttpRequest *req, AsyncCompletionGenericDelegate *delegate);
        //! notify handlers
        virtual void OnDisconnect();

        //! ###
        //! called in thread pool
        virtual void OnHandleFinished(Details::AbstractHttpHandler *handler);

        //! send redirect request
        virtual void SendRedirect(const HttpRequest *req,
            AsyncCompletionGenericDelegate *delegate,
            RedirectCompletionGenericDelegate *redirectDelegate,
            const HttpResponseHeaders& headers);
    };

    namespace Details
    {
        class WriteableResponseStream;
        class DefaultResponseCompletionHandler : public AsyncHandler<HttpResponse>
        {
        private:
            HttpResponseHeaders m_headers;
            WriteableResponseStream *m_responseStream;

        public:
            virtual ~DefaultResponseCompletionHandler() {}

        public:
            virtual void OnRequestDataFilled() {}
            virtual void OnHeaderAvailable(const Net::HttpResponseHeaders& headers);
            virtual void OnBodyAvailable(InputStream& inputStream);
            virtual Exception *OnException(Exception *ex) throw() { return ex; }

        public:
            virtual HttpResponse OnCompleted();
        };

        class OneTimeStream : public InputStream
        {
        private:
            uint32_t m_buffLength;

        public:
            uint32_t m_readableLength;
            uint8_t *m_buffer;
            int64_t m_contentLength;

        public:
            OneTimeStream()
                : m_buffLength(4096)
                , m_contentLength(0)
                , m_buffer(NULL)
                , m_readableLength(0)
            {}
            ~OneTimeStream()
            {
                Deallocate();
            }

        public:
            virtual int64_t GetTotal() const { return m_contentLength; }

        public:
            virtual int64_t GetAvailCount() const { return m_readableLength; }
            virtual uint32_t Read(uint8_t *buffer, uint32_t read);

        public:
            uint32_t BufferLength() const { return m_buffLength; }

        public:
            void Allocate();
            void Deallocate();
        };

        uint32_t OneTimeStream::Read(uint8_t *buffer, uint32_t read)
        {
            uint32_t aboutToRead = min(read, m_readableLength);
            ::memcpy(buffer, m_buffer, aboutToRead);

            return aboutToRead;
        }

        void OneTimeStream::Allocate()
        {
            if (!m_buffer)
                m_buffer = static_cast<uint8_t *>(::malloc(m_buffLength));
        }

        void OneTimeStream::Deallocate()
        {
            if (m_buffer)
            {
                ::free(m_buffer);
                m_buffer = NULL;
            }
        }

        class WriteableResponseStream : public InputStream
        {
        public:
            virtual WriteableResponseStream& operator << (InputStream& is) = 0;
            virtual void OnWriteFinished() = 0;
        };

        class TempFileWriteableResponseStream : public WriteableResponseStream
        {
        private:
            uint32_t m_bufferLength;
            uint8_t *m_buffer;

            mutable HANDLE m_hFile; //! when calling GetTotal the handle of file will be refreshed
            String m_filePath;

        private:
            uint64_t m_seeker;

        public:
            TempFileWriteableResponseStream();
            virtual ~TempFileWriteableResponseStream();

        public:
            virtual int64_t GetAvailCount() const;
            virtual uint32_t Read(uint8_t *buffer, uint32_t read);
            virtual int64_t GetTotal() const;

        public:
            virtual WriteableResponseStream& operator << (InputStream& is);
            virtual void OnWriteFinished();

        private:
            void Dispose();
        };

        class SimpleBufferWriteableResponseStream : public WriteableResponseStream
        {
        private:
            uint32_t m_offset;
            uint32_t m_bufferLength;
            uint8_t *m_buffer;

        public:
            SimpleBufferWriteableResponseStream(uint32_t aboutToRead)
                : m_offset(0)
                , m_bufferLength(aboutToRead)
                , m_buffer(NULL)
            {}

            virtual ~SimpleBufferWriteableResponseStream()
            {
                if (m_buffer)
                    ::free(m_buffer);
            }

        public:
            virtual int64_t GetTotal() const { return m_bufferLength; }

            virtual int64_t GetAvailCount() const
            {
                return m_offset;
            }

            virtual uint32_t Read(uint8_t *buffer, uint32_t read)
            {
                if (NULL == m_buffer || 0 == m_offset)
                    return 0;

                uint32_t readCount = min(m_offset, read);
                ::memcpy(buffer, m_buffer + m_bufferLength - m_offset, readCount);

                m_offset -= readCount;

                return readCount;
            }

        public:
            virtual WriteableResponseStream& operator << (InputStream& is)
            {
                if (m_offset < m_bufferLength)
                {
                    if (NULL == m_buffer)
                        m_buffer = static_cast<uint8_t *>(::malloc(m_bufferLength));

                    m_offset += is.Read(m_buffer + m_offset, m_bufferLength - m_offset);
                }

                return *this;
            }

            virtual void OnWriteFinished()
            {
                //! do nothing
            }
        };

        TempFileWriteableResponseStream::TempFileWriteableResponseStream()
            : m_bufferLength(4096)
            , m_buffer(NULL)
            , m_hFile(NULL)
            , m_filePath()
            , m_seeker(0)
        {}

        TempFileWriteableResponseStream::~TempFileWriteableResponseStream()
        {
            Dispose();

            if (m_buffer)
                ::free(m_buffer);
        }

        int64_t TempFileWriteableResponseStream::GetAvailCount() const
        {
            return m_seeker;
        }

        uint32_t TempFileWriteableResponseStream::Read(uint8_t *buffer, uint32_t read)
        {
            uint32_t readCount = static_cast<uint32_t>(min(m_seeker, static_cast<uint64_t>(read)));
            if (readCount)
            {
                if (m_hFile == NULL)
                {
                    HANDLE hFile = ::CreateFile(m_filePath.c_str(), GENERIC_READ, FILE_SHARE_DELETE | FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (NULL == hFile || INVALID_HANDLE_VALUE == hFile)
                    {
                        throw IOException();
                    }

                    m_hFile = hFile;
                }

                DWORD alreadRead = 0;
                if (!::ReadFile(m_hFile, buffer, readCount, &alreadRead, NULL))
                {
                    throw IOException();
                }

                readCount = alreadRead;
                //! update seeker
                m_seeker -= alreadRead;
            }

            return readCount;
        }

        WriteableResponseStream& TempFileWriteableResponseStream::operator << (InputStream& is)
        {
            if (m_hFile == NULL)
            {
                WCHAR lpTempDir[MAX_PATH - 14] = { 0 };
                DWORD dwPathLen = ::GetTempPathW(MAX_PATH - 14, lpTempDir);

                if (dwPathLen > 0 && dwPathLen < MAX_PATH - 14)
                {
                    WCHAR lpTempFile[MAX_PATH] = { 0 };
                    if (::GetTempFileName(lpTempDir, L"XXX", 0, lpTempFile))
                    {
                        HANDLE hFile = ::CreateFile(lpTempFile, GENERIC_WRITE, FILE_SHARE_DELETE | FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                        if (NULL == hFile || INVALID_HANDLE_VALUE == hFile)
                        {
                            throw IOException();
                        }

                        m_filePath = lpTempFile;
                        m_hFile = hFile;
                    }
                }
            }

            if (m_buffer == NULL)
            {
                m_buffer = static_cast<uint8_t *>(::malloc(m_bufferLength));
            }

            uint32_t readCount = is.Read(m_buffer, m_bufferLength);
            DWORD dwWriteCount = 0;
            if (!::WriteFile(m_hFile, m_buffer, readCount, &dwWriteCount, NULL))
            {
                throw IOException();
            }

            m_seeker += dwWriteCount;

            return *this;
        }

        void TempFileWriteableResponseStream::OnWriteFinished()
        {
            if (m_hFile)
            {
                ::FlushFileBuffers(m_hFile);
                ::CloseHandle(m_hFile);

                m_hFile = NULL;
            }
        }

        void TempFileWriteableResponseStream::Dispose()
        {
            if (m_hFile)
            {
                ::CloseHandle(m_hFile);
                m_hFile = NULL;
            }

            if (!m_filePath.empty())
            {
                ::DeleteFile(m_filePath.c_str());
            }
        }

        int64_t TempFileWriteableResponseStream::GetTotal() const
        {
            int64_t totalSize = 0;
            if (NULL == m_hFile && !m_filePath.empty())
            {
                HANDLE hFile = ::CreateFile(m_filePath.c_str(), GENERIC_READ, FILE_SHARE_DELETE | FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                if (NULL == hFile || INVALID_HANDLE_VALUE == hFile)
                {
                    throw IOException();
                }

                LARGE_INTEGER fileSize = { 0 };
                if (!::GetFileSizeEx(m_hFile, &fileSize))
                {
                    throw IOException();
                }

                m_hFile = hFile;
                totalSize = fileSize.QuadPart;
            }

            return totalSize;
        }

        void DefaultResponseCompletionHandler::OnHeaderAvailable(const Net::HttpResponseHeaders& headers)
        {
            int64_t length = headers.GetContentLength();

            if (length > 0 && length < 1024 * 1024 * 2) //! lower than 2 MB
            {
                m_responseStream = new SimpleBufferWriteableResponseStream(static_cast<uint32_t>(length));
            }
            else
            {
                //! unknown length or larger than 2MB
                m_responseStream = new TempFileWriteableResponseStream;
            }

            m_headers = headers;
        }

        void DefaultResponseCompletionHandler::OnBodyAvailable(InputStream& inputStream)
        {
            *m_responseStream << inputStream;
        }

        HttpResponse DefaultResponseCompletionHandler::OnCompleted()
        {
            m_responseStream->OnWriteFinished();
            return HttpResponse(m_headers, m_responseStream);
        }

        static Exception *ConvertLastError(DWORD dwErr)
        {
            //! check error
            if (ERROR_WINHTTP_INVALID_URL == dwErr)
                return new InvalidURLFormatException();

            return new NetException(dwErr);
        }

        class AbstractHttpHandler
        {
        public:
            const HttpRequest *m_request;

        protected:
            //! when closed, all those will be NULL
            HINTERNET m_hRequest;

            AsyncCompletionGenericDelegate *m_completionAsyncHandler;

            AsyncCompletionGenericDelegate *m_normalAsyncHandler;
            RedirectCompletionGenericDelegate *m_redirectDelegate;

            HttpSession::Private *m_sessionImpl;

        protected:
            //! deallocate when closed 
            OneTimeStream m_bufferStream;

            HttpResponseHeaders m_headers;

        public:
            AbstractHttpHandler(HINTERNET hReq, const HttpRequest *req, AsyncCompletionGenericDelegate *delegate, HttpSession::Private *sessionImpl)
                : m_hRequest(hReq)
                , m_redirectDelegate(DefaultRedirectCompletionGenericDelegate::GetDefaultDelegate())
                , m_completionAsyncHandler(delegate)
                , m_normalAsyncHandler(delegate)
                , m_request(req)
                , m_sessionImpl(sessionImpl)
            {}

            AbstractHttpHandler(HINTERNET hReq, const HttpRequest *req, AsyncCompletionGenericDelegate *delegate, RedirectCompletionGenericDelegate *redirectDelegate, HttpSession::Private *sessionImpl)
                : m_hRequest(hReq)
                , m_redirectDelegate(redirectDelegate)
                , m_completionAsyncHandler(delegate)
                , m_normalAsyncHandler(delegate)
                , m_request(req)
                , m_sessionImpl(sessionImpl)
            {}

            virtual ~AbstractHttpHandler()
            {
            }

        public:
            //! 
            virtual void Terminate();

            /**
                When finished, the handler may(will if synchronous) be deleted, which means no more call after sending request
                */
            virtual void OnSendingRequest() = 0;
            /**
                Events
                NO more callings after any of those methods showing below
                */
            //! on request already sent
            void OnRequestSent();
            void OnError(WINHTTP_ASYNC_RESULT *result);
            //! send request stream
            //! len means length already sent
            void OnWriteData(DWORD len);

            void OnReadHeader();
            void OnReadData(DWORD len);

            //! on handler closed
            //! no more callings
            void OnFinished();

        protected:
            //! close the handler
            //! request handler is invalid
            //! exception == nullptr means success
            virtual void OnClose(Exception *exception);

            virtual void OnReadingData() = 0;

        private:
            void OnTerminated();
            /**
                NO more callings after any of those methods showing below
                */
            //! start to receive reponse header
            void OnReceiveResponse();

            void OnWritingData(InputStream *);
        };

        static void CALLBACK _Callback(_In_ HINTERNET hInternet,
            _In_ DWORD_PTR dwContext,
            _In_ DWORD dwInternetStatus,
            _In_opt_ LPVOID lpvStatusInformation,
            _In_ DWORD dwStatusInformationLength)
        {
            AbstractHttpHandler *handler = (AbstractHttpHandler *)dwContext;

            switch (dwInternetStatus)
            {
                /**
                    Response
                    */
            case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
                handler->OnReadData(dwStatusInformationLength);
                break;

            case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
                handler->OnReadHeader();
                break;

                /**
                    Request
                    */
            case WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE:
                handler->OnWriteData(dwStatusInformationLength);
                break;

            case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
                handler->OnRequestSent();
                break;

            case WINHTTP_CALLBACK_STATUS_REQUEST_SENT:
                break;

                /**
                    Error
                    */
            case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
                handler->OnError(static_cast<WINHTTP_ASYNC_RESULT*>(lpvStatusInformation));
                break;

            case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
                handler->OnFinished();
                break;

            default:
                //TRACE(_T("Unknown status:%08X"), dwInternetStatus);
                break;
            }
        }

        void AbstractHttpHandler::OnFinished()
        {
            //! safe quit will clean request value
            if (m_hRequest)
                OnTerminated();

            //! remove from handlers manager
            m_sessionImpl->OnHandleFinished(this);

            delete this;
        }

        void AbstractHttpHandler::Terminate()
        {
            ::WinHttpCloseHandle(m_hRequest);
        }

        void AbstractHttpHandler::OnError(WINHTTP_ASYNC_RESULT *result)
        {
            //! close handler failed
            if (NULL == m_hRequest)
            {
                OnFinished();
            }
            else
            {
                OnClose(ConvertLastError(result->dwError));
            }
        }

        void AbstractHttpHandler::OnRequestSent()
        {
            //! check request body has any data to be sent
            InputStream *bodyStream = m_request->GetRequestBodyStream();
            if (!bodyStream || bodyStream->GetAvailCount() == 0)
            {
                OnReceiveResponse();
            }
            else
            {
                //! start write data
                if (m_bufferStream.m_buffer == NULL)
                    m_bufferStream.Allocate();

                OnWritingData(bodyStream);
            }
        }

        void AbstractHttpHandler::OnReceiveResponse()
        {
            if (!::WinHttpReceiveResponse(m_hRequest, 0))
            {
                OnClose(ConvertLastError(::GetLastError()));
            }
        }

        void AbstractHttpHandler::OnWriteData(DWORD completedLength)    //! ignored for now
        {
            InputStream *bodyStream = m_request->GetRequestBodyStream();

            //! read finished
            if (bodyStream->GetAvailCount() == 0)
            {
                OnReceiveResponse();
            }
            else
            {
                OnWritingData(bodyStream);
            }
        }

        void AbstractHttpHandler::OnWritingData(InputStream *bodyStream)
        {
            try
            {
                uint32_t readCount = bodyStream->Read(m_bufferStream.m_buffer, m_bufferStream.BufferLength());

                if (!::WinHttpWriteData(m_hRequest, m_bufferStream.m_buffer, readCount, NULL))
                {
                    OnClose(ConvertLastError(::GetLastError()));
                }
            }
            catch (const Exception& ex)
            {
                OnClose(ex.Clone());
            }
        }

        void AbstractHttpHandler::OnReadData(DWORD len)
        {
            if (len == 0)
            {
                //!	finished
                OnClose(NULL);
            }
            else
            {
                m_bufferStream.m_readableLength = len;

                try
                {
                    m_completionAsyncHandler->OnBodyAvailable(m_bufferStream);

                    //! read next piece
                    OnReadingData();
                }
                catch (const Exception& ex)
                {
                    OnClose(ex.Clone());
                }
            }
        }

        void AbstractHttpHandler::OnReadHeader()
        {
            DWORD statusCode = 0;

            DWORD dwStatusCodeSize = sizeof(statusCode);
            if (!WinHttpQueryHeaders(m_hRequest
                , WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER
                , WINHTTP_HEADER_NAME_BY_INDEX
                , &statusCode
                , &dwStatusCodeSize
                , WINHTTP_NO_HEADER_INDEX))
            {
                OnClose(ConvertLastError(::GetLastError()));
                return;
            }

            WCHAR wszContentLength[32] = { 0 };
            DWORD dwBufferSize = sizeof(wszContentLength);
            if (!WinHttpQueryHeaders(m_hRequest
                , WINHTTP_QUERY_CONTENT_LENGTH
                , WINHTTP_HEADER_NAME_BY_INDEX
                , wszContentLength
                , &dwBufferSize
                , WINHTTP_NO_HEADER_INDEX))
            {
                DWORD dwError = ::GetLastError();
                if (ERROR_HTTP_HEADER_NOT_FOUND != dwError)
                {
                    OnClose(ConvertLastError(dwError));
                    return;
                }
            }

            //Update the output parameter
            int64_t contentLength = _wtoi64(wszContentLength);

            //!	query all
            {
                DWORD dwSize = 0;
                WinHttpQueryHeaders(m_hRequest
                    , WINHTTP_QUERY_RAW_HEADERS
                    , WINHTTP_HEADER_NAME_BY_INDEX
                    , NULL, &dwSize, WINHTTP_NO_HEADER_INDEX);

                // Allocate memory for the buffer.
                if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
                {
                    WCHAR *lpOutBuffer = new WCHAR[dwSize / sizeof(WCHAR)];

                    // Now, use WinHttpQueryHeaders to retrieve the header.
                    if (!WinHttpQueryHeaders(m_hRequest
                        , WINHTTP_QUERY_RAW_HEADERS
                        , WINHTTP_HEADER_NAME_BY_INDEX
                        , lpOutBuffer, &dwSize, WINHTTP_NO_HEADER_INDEX))
                    {
                        delete[] lpOutBuffer;

                        OnClose(ConvertLastError(::GetLastError()));
                        return;
                    }

                    HttpResponseHeaders headers(lpOutBuffer, static_cast<StatusCode::Value>(statusCode), contentLength);
                    m_headers = headers;

                    //! check if redirect
                    if (m_headers.GetStatusCode() == StatusCode::Moved_Permanently ||
                        m_headers.GetStatusCode() == StatusCode::Found ||
                        m_headers.GetStatusCode() == StatusCode::See_Other ||
                        m_headers.GetStatusCode() == StatusCode::Use_Proxy ||
                        m_headers.GetStatusCode() == StatusCode::Temporary_Redirect)
                    {
                        const String& redirectURL = m_headers.GetHead(L"Location");
                        m_redirectDelegate->SetLocation(redirectURL);
                        m_completionAsyncHandler = m_redirectDelegate;
                    }

                    try
                    {
                        m_completionAsyncHandler->OnHeaderAvailable(m_headers);
                    }
                    catch (const Exception& ex)
                    {
                        OnClose(ex.Clone());
                        return;
                    }

                    //! update body
                    m_bufferStream.m_contentLength = contentLength;

                    OnReadingData();
                }
            }
        }

        void AbstractHttpHandler::OnClose(Exception *exception)
        {
            //! send results
            if (NULL != exception)
            {
                m_completionAsyncHandler->OnError(exception);
            }
            else
            {
                //! check if redirect
                if (m_completionAsyncHandler == m_redirectDelegate && m_redirectDelegate->PerformRedirecting())
                {
                    //!
                    m_sessionImpl->SendRedirect(m_request, m_normalAsyncHandler, m_redirectDelegate, m_headers);
                }
                else
                {
                    m_completionAsyncHandler->OnCompleted();
                }
                //! clean response and headers
                //! TO-DO
            }

            //! clean the scoped variables
            m_request = NULL;
            m_completionAsyncHandler = NULL;

            //! close
            HINTERNET hReq = m_hRequest;
            m_hRequest = NULL;

            ::WinHttpCloseHandle(hReq);
        }

        void AbstractHttpHandler::OnTerminated()
        {
            m_completionAsyncHandler->OnError(new ConnectionTerminatedException);

            //! clean the scoped variables
            m_request = NULL;
            m_completionAsyncHandler = NULL;
        }

        class SyncHttpHandler : public AbstractHttpHandler
        {
        public:
            SyncHttpHandler(HINTERNET hReq, const HttpRequest *req, AsyncCompletionGenericDelegate *delegate, HttpSession::Private*session)
                : AbstractHttpHandler(hReq, req, delegate, session)
            {}

            virtual ~SyncHttpHandler()
            {}

        public:
            virtual void OnSendingRequest()
            {
                const String& header = m_request->GetHeadersString();

                //! #
                DWORD dwTotal = m_request->GetRequestBodyStream() == NULL ? 0 : static_cast<DWORD>(m_request->GetRequestBodyStream()->GetTotal());

                if (!WinHttpSendRequest(m_hRequest,
                    header.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header.c_str(), -1,
                    WINHTTP_NO_REQUEST_DATA, 0,
                    dwTotal, NULL))// context
                {
                    OnClose(ConvertLastError(::GetLastError()));
                    return;
                }

                OnRequestSent();

                //! completion has been cleaned
                if (m_completionAsyncHandler)
                {
                    OnReadHeader();
                }

                //! delete itself
                OnFinished();
            }

        protected:
            virtual void OnReadingData()
            {
                if (m_bufferStream.m_buffer == NULL)
                    m_bufferStream.Allocate();

                DWORD dwAvailCount = 0;
                if (!WinHttpQueryDataAvailable(m_hRequest, &dwAvailCount))
                {
                    OnClose(ConvertLastError(::GetLastError()));
                    return;
                }

                DWORD dwRead = 0;
                if (!::WinHttpReadData(m_hRequest, m_bufferStream.m_buffer, min(dwAvailCount, m_bufferStream.BufferLength()), &dwRead))
                {
                    OnClose(ConvertLastError(::GetLastError()));
                    return;
                }

                OnReadData(dwRead);
            }
        };

        class AsyncHttpHandler : public AbstractHttpHandler
        {
        private:
            REF m_isClosed;

        public:
            AsyncHttpHandler(HINTERNET hReq, const HttpRequest *req, AsyncCompletionGenericDelegate *delegate, HttpSession::Private*session)
                : AbstractHttpHandler(hReq, req, delegate, session)
                , m_isClosed(0)
            {}

            AsyncHttpHandler(HINTERNET hReq, const HttpRequest *req, AsyncCompletionGenericDelegate *delegate, RedirectCompletionGenericDelegate *redirectDelegate, HttpSession::Private*session)
                : AbstractHttpHandler(hReq, req, delegate, redirectDelegate, session)
                , m_isClosed(0)
            {}

        public:
            virtual void OnSendingRequest()
            {
                const String& header = m_request->GetHeadersString();

                WINHTTP_STATUS_CALLBACK installed = WinHttpSetStatusCallback(m_hRequest, _Callback, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, 0);
                DWORD dwError = ::GetLastError();
                if (dwError != ERROR_SUCCESS)
                {
                    OnClose(ConvertLastError(dwError));
                    return;
                }

                //! #
                DWORD dwTotal = m_request->GetRequestBodyStream() == NULL ? 0 : static_cast<DWORD>(m_request->GetRequestBodyStream()->GetTotal());

                if (!WinHttpSendRequest(m_hRequest,
                    header.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header.c_str(), -1,
                    WINHTTP_NO_REQUEST_DATA, 0,
                    dwTotal, (UINT_PTR)this))// context
                {
                    OnClose(ConvertLastError(::GetLastError()));
                    return;
                }
            }

            virtual void Terminate()
            {
                //! if has set(value equals 1)
                if (CompareExchange(&m_isClosed, 1, 0) == 1)
                    return;

                __super::Terminate();
            }

        protected:
            void OnClose(Exception *exception)
            {
                //! if has set(value equals 1)
                if (CompareExchange(&m_isClosed, 1, 0) == 1)
                {
                    //! free the exception
                    delete exception;
                    return;
                }

                __super::OnClose(exception);
            }

            virtual void OnReadingData()
            {
                if (m_bufferStream.m_buffer == NULL)
                    m_bufferStream.Allocate();

                if (!::WinHttpReadData(m_hRequest, m_bufferStream.m_buffer, m_bufferStream.BufferLength(), 0))
                {
                    OnClose(ConvertLastError(::GetLastError()));
                }
            }
        };

    }
}

namespace Net
{
    class HttpResponseHeaders::RawHeaders
    {
    public:
        AtomicRef m_ref;
        const wchar_t *m_rawHeadersData;

    public:
        RawHeaders(const wchar_t *data)
            : m_ref()
            , m_rawHeadersData(data)
        {
        }

        ~RawHeaders()
        {
            if (m_rawHeadersData)
                delete[] m_rawHeadersData;
        }
    };


    class HttpResponse::PrivateData
    {
    public:
        AtomicRef m_ref;

        HttpResponseHeaders m_headers;
        InputStream *m_bodyStream;

    public:
        PrivateData(const HttpResponseHeaders& headers, InputStream *bodyStream)
            : m_ref()
            , m_headers(headers)
            , m_bodyStream(bodyStream)
        {
        }

        ~PrivateData()
        {
            if (m_bodyStream)
                delete m_bodyStream;
        }
    };

    HttpResponse::HttpResponse()
        : m_d(NULL)
    {}

    HttpResponse::HttpResponse(const HttpResponseHeaders& headers, InputStream *takenOwnershipStream)
        : m_d(NULL)
    {
        m_d = new PrivateData(headers, takenOwnershipStream);
    }

    HttpResponse::HttpResponse(const HttpResponse& resp)
        : m_d(resp.m_d)
    {
    }

    HttpResponse& HttpResponse::operator = (const HttpResponse& t)
    {
        m_d = t.m_d;
        return *this;
    }

    HttpResponse::~HttpResponse()
    {
    }

    InputStream *HttpResponse::GetBodyStream() const
    {
        return m_d ? m_d->m_bodyStream : NULL;
    }

    HttpResponseHeaders HttpResponse::GetHeaders() const
    {
        return m_d ? m_d->m_headers : HttpResponseHeaders();
    }

    HttpResponseHeaders::HttpResponseHeaders()
        : m_statusCode(StatusCode::OK)
        , m_rawHeaders(NULL)
        , m_contentLength(0)
    {

    }

    HttpResponseHeaders::HttpResponseHeaders(const wchar_t *rawHeader)
        : m_statusCode(StatusCode::OK)
        , m_rawHeaders(NULL)
        , m_contentLength(0)
    {
        m_rawHeaders = new RawHeaders(rawHeader);
    }

    HttpResponseHeaders::HttpResponseHeaders(const wchar_t *rawHeader, const StatusCode& statusCode, const int64_t& contentLength)
        : m_statusCode(statusCode)
        , m_rawHeaders(NULL)
        , m_contentLength(contentLength)
    {
        m_rawHeaders = new RawHeaders(rawHeader);
    }

    HttpResponseHeaders::HttpResponseHeaders(const HttpResponseHeaders& r)
        : m_statusCode(r.m_statusCode)
        , m_rawHeaders(r.m_rawHeaders)
        , m_contentLength(r.m_contentLength)
    {
    }

    HttpResponseHeaders& HttpResponseHeaders::operator = (const HttpResponseHeaders& t)
    {
        m_rawHeaders = t.m_rawHeaders;
        m_statusCode = t.m_statusCode;
        m_contentLength = t.m_contentLength;

        return *this;
    }

    HttpResponseHeaders::~HttpResponseHeaders()
    {
    }

    const wchar_t *HttpResponseHeaders::Get(const wchar_t *name, size_t& out_len) const
    {
        const wchar_t *current = m_rawHeaders->m_rawHeadersData;

        if (NULL != current)
        {
            while (true)
            {
                size_t len = std::wcslen(current);

                const wchar_t *found = std::wcsstr(current, name);

                //! found
                if (found == current)
                {
                    out_len = len;
                    break;
                }

                current += (len + 1);
                if (L'\0' == *current)
                {
                    current = NULL;
                    break;
                }
            }
        }

        return current;
    }

    String  HttpResponseHeaders::GetHead(const wchar_t *headName) const
    {
        size_t entryLength = 0;
        const wchar_t *headEntry = Get(headName, entryLength);

        if (NULL != headEntry && 0 != entryLength)
        {
            String::size_type offset = std::wcslen(headName);
            const wchar_t *colon = std::wcsstr(headEntry + offset, L":"); // find separator colon

            if (NULL != colon)
            {
                const wchar_t *substart = colon + 1;
                const wchar_t *eol = headEntry + entryLength;

                // trim start
                while (substart < eol && (*substart) == ' ')
                    ++substart;

                // trim end
                while (substart < eol && *(eol - 1) == ' ')
                    --eol;

                return String(substart, eol);
            }
        }

        return String();
    }

    String RequestHeadersBuilder::ToString() const
    {
        if (m_headers.empty())
            return String();

        std::wstringstream ss;

        Headers::const_iterator it = m_headers.begin();
        for (; it != m_headers.end(); ++it)
        {
            ss << it->first << L": " << it->second << L"\r\n";
        }

        ss << L"\r\n";
        return ss.str();
    }

    RequestHeadersBuilder& RequestHeadersBuilder::operator << (const Headers::value_type& eachHeader)
    {
        m_headers.insert(eachHeader);
        return *this;
    }

    HINTERNET HttpSession::Private::AcquireRequest(const URL& url, const HttpVerb& verb)
    {
        URL_COMPONENTS urlComp;

        ZeroMemory(&urlComp, sizeof(urlComp));
        urlComp.dwStructSize = sizeof(urlComp);

        urlComp.dwSchemeLength = (DWORD)-1;
        urlComp.dwHostNameLength = (DWORD)-1;
        urlComp.dwUrlPathLength = (DWORD)-1;
        urlComp.dwExtraInfoLength = (DWORD)-1;

        // Crack the URL.
        if (!WinHttpCrackUrl(url.c_str(), url.size(), 0, &urlComp))
        {
            throw InvalidURLFormatException();
        }

        String host(urlComp.lpszHostName, urlComp.dwHostNameLength);
        String path(urlComp.lpszUrlPath, urlComp.dwUrlPathLength);
        if (NULL != urlComp.lpszExtraInfo)
            path.append(String(urlComp.lpszExtraInfo, urlComp.dwExtraInfoLength));

        HttpSecurityOptions opts;
        opts.isHttps = urlComp.nScheme == INTERNET_SCHEME_HTTPS;

        return OpenRequest(AcquireConnection(host, urlComp.nPort), path, verb, opts);
    }

    HINTERNET HttpSession::Private::AcquireConnection(const String& host, uint16_t port)
    {
        if (!m_hSession)
        {
            m_hSession = ::WinHttpOpen(L"HTTP/1.1"
                , WINHTTP_ACCESS_TYPE_DEFAULT_PROXY
                , WINHTTP_NO_PROXY_NAME
                , WINHTTP_NO_PROXY_BYPASS
                , m_config.IsAsync ? WINHTTP_FLAG_ASYNC : NULL);  //! async

            //! config
            WinHttpSetTimeouts(m_hSession, 3000, 3000, 10000, 10000);

            if (!m_config.IsAutoRedirectEnabled)
            {
                DWORD redirectFeature = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
                WinHttpSetOption(m_hSession, WINHTTP_OPTION_REDIRECT_POLICY, &redirectFeature, sizeof(redirectFeature));
            }
        }

        Details::HostConnections::iterator found = m_connections.find(host);

        HINTERNET connection = NULL;
        //! no such connection
        if (found == m_connections.end())
        {
            connection = WinHttpConnect(m_hSession, host.c_str(), port, 0);
            if (!connection)
            {
                throw ConnectionFailedException();
            }
        }
        else
        {
            connection = found->second;
        }

        return connection;
    }

    HINTERNET HttpSession::Private::OpenRequest(HINTERNET hConnection, const String& path, HttpVerb verb, const HttpSecurityOptions& securityOpts)
    {
        static const LPWSTR VerbMapper[] = { L"GET", L"POST", L"DELETE", L"PUT" };

        HINTERNET hRequest = WinHttpOpenRequest(hConnection, VerbMapper[verb], path.c_str(),
            NULL,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            securityOpts.isHttps ? WINHTTP_FLAG_SECURE : 0);

        if (securityOpts.isHttps)
        {
            DWORD options = SECURITY_FLAG_IGNORE_CERT_CN_INVALID
                | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID
                | SECURITY_FLAG_IGNORE_UNKNOWN_CA;
            ::WinHttpSetOption(hRequest,
                WINHTTP_OPTION_SECURITY_FLAGS,
                (LPVOID)&options,
                sizeof(DWORD));
        }

        return hRequest;
    }

    void HttpSession::Private::Disconnect()
    {
        if (m_hSession)
        {
            OnDisconnect();

            Details::HostConnections::iterator it = m_connections.begin();
            for (; it != m_connections.end(); ++it)
            {
                ::WinHttpCloseHandle(it->second);
            }

            //
            ::WinHttpCloseHandle(m_hSession);
            m_hSession = NULL;
        }
    }

    //
    //  ###
    //  DANGER
    //  When operation is sync, Terminate will trigger handler erasing
    //
    void HttpSession::Private::OnDisconnect()
    {
        //! under the condition the type of handlers is linked list
        Details::HttpHandlers::iterator it = m_handlers.begin();

        while (m_handlers.end() != it)
        {
            Details::HttpHandlers::iterator cur = it++;
            (*cur)->Terminate();
        }
    }

    void HttpSession::Private::OnHandleFinished(Details::AbstractHttpHandler *handler)
    {
        Details::HttpHandlers::iterator found = std::find(m_handlers.begin(), m_handlers.end(), handler);
        if (found != m_handlers.end())
            m_handlers.erase(found);
    }

    //! send request
    void HttpSession::Private::SendRequest(HINTERNET hReq, const HttpRequest *req, AsyncCompletionGenericDelegate *delegate)
    {
        if (m_config.IsAsync)
        {
            // non-lock async http session is forbidden
            throw std::logic_error("non-lock async http session is forbidden");
        }

        Details::AbstractHttpHandler *handler = new Details::SyncHttpHandler(hReq, req, delegate, this);
        m_handlers.push_back(handler);

        handler->OnSendingRequest();
    }

    void HttpSession::Private::SendRedirect(const HttpRequest *req,
        AsyncCompletionGenericDelegate *delegate,
        RedirectCompletionGenericDelegate *redirectDelegate,
        const HttpResponseHeaders& headers)
    {
        const String& location = headers.GetHead(L"Location");

        bool reissuingRequest = headers.GetStatusCode() == StatusCode::See_Other;

        HINTERNET hReq = NULL;
        try
        {
            hReq = AcquireRequest(location, reissuingRequest ? HttpVerb::Get : req->GetVerb());
            if (NULL == hReq)
            {
                throw ConnectionFailedException();
            }
        }
        catch (const Exception& ex)
        {
            delegate->OnError(ex.Clone());
            return;
        }

        //! the redirect request will remain valid till the end
        //! however, the url and verb still be the first time's
        SendRequest(hReq, req, delegate);
    }

    LockHttpSessionPrivate::LockHttpSessionPrivate()
        : HttpSession::Private()
        , m_lock()
        , m_disconnectedEvent(true)    //! signaled
    {
        }

    LockHttpSessionPrivate::LockHttpSessionPrivate(const HttpSessionConfig& config)
        : HttpSession::Private(config)
        , m_lock()
        , m_disconnectedEvent(true)    //! signaled
    {
        }

    void LockHttpSessionPrivate::SendRequest(HINTERNET hReq, const HttpRequest *req, AsyncCompletionGenericDelegate *delegate)
    {
        Details::AbstractHttpHandler *handler = NULL;
        if (m_config.IsAsync)
            handler = new Details::AsyncHttpHandler(hReq, req, delegate, this);
        else
            handler = new Details::SyncHttpHandler(hReq, req, delegate, this);

        {
            AutoLock<CriticalSection> locker(&m_lock);
            m_handlers.push_back(handler);
        }

        handler->OnSendingRequest();
    }

    void LockHttpSessionPrivate::OnDisconnect()
    {
        //! reset the event
        m_disconnectedEvent.Reset();

        {
            AutoLock<CriticalSection> locker(&m_lock);
            __super::OnDisconnect();

            //! no more handlers
            //! just signal the event
            if (m_handlers.empty())
                m_disconnectedEvent.Signal();
        }

        //! wait till finished
        m_disconnectedEvent.Wait(INFINITE);
    }

    void LockHttpSessionPrivate::SendRedirect(const HttpRequest *req,
        AsyncCompletionGenericDelegate *delegate,
        RedirectCompletionGenericDelegate *redirectDelegate,
        const HttpResponseHeaders& headers)
    {
        //! terminating 
        if (!m_disconnectedEvent.IsSignaled())
            return;

        AutoLock<CriticalSection> locker(&m_lock);
        __super::SendRedirect(req, delegate, redirectDelegate, headers);
    }

    void LockHttpSessionPrivate::OnHandleFinished(Details::AbstractHttpHandler *handler)
    {
        AutoLock<CriticalSection> locker(&m_lock);
        __super::OnHandleFinished(handler);

        if (m_handlers.empty())
            m_disconnectedEvent.Signal();
    }

    HttpSession::HttpSession()
        : m_sessionImpl(new LockHttpSessionPrivate)
    {}

    HttpSession::HttpSession(const HttpSessionConfig& config)
        : m_sessionImpl(config.IsAsync ? new LockHttpSessionPrivate(config) : new HttpSession::Private(config))
    {}

    HttpSession::~HttpSession()
    {
        Disconnect();

        if (m_sessionImpl)
            delete m_sessionImpl;
    }

    void HttpSession::Disconnect()
    {
        m_sessionImpl->Disconnect();
    }

    void HttpSession::SendRequest(const HttpRequest *req, AsyncCompletionGenericDelegate *delegate)
    {
        HINTERNET hReq = m_sessionImpl->AcquireRequest(req->GetURL(), req->GetVerb());

        if (NULL == hReq)
        {
            throw ConnectionFailedException();
        }

        m_sessionImpl->SendRequest(hReq, req, delegate);
    }

    AsyncHandler<HttpResponse> *HttpClient::AcquireDefaultHandler()
    {
        return new Details::DefaultResponseCompletionHandler();
    }
}

