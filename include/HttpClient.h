#ifndef HTTPCLIENT_H
#define HTTPCLIENT_H

#include "Promise.h"

#include "HttpClientModule.h"
#include "ScopedPointer.h"

class SimpleStringInputStream : public InputStream
{
private:
    std::string::size_type m_offset;
    std::string m_buffer;

public:
    explicit SimpleStringInputStream(const String& str);
    explicit SimpleStringInputStream(const std::string& str);

public:
    virtual int64_t GetAvailCount() const;
    virtual uint32_t Read(uint8_t *buffer, uint32_t read);
    virtual int64_t GetTotal() const;
};

class CoreEventHandler
{
public:
    virtual ~CoreEventHandler() {}

public:
    virtual void OnRequestDataFilled() {}
    virtual void OnHeaderAvailable(const Net::HttpResponseHeaders& headers) = 0;
    virtual void OnBodyAvailable(InputStream& inputStream) {}

    //
    // exception interceptor
    // though u cannot reject the exception, u can wrap the exception with ur own data strcuture
    // u can delete the given exception and replace with new one
    // when returned, u shall never own that exception any more
    // Besides, return value cannot be null
    virtual Exception *OnException(Exception *ex) throw() { return ex; }
};

//
//  NB
//  when u implement this interface
//  u will receive notification carried with redirecting header and body(NOT redirected site's)
//
class RedirectHandleable
{
public:
    virtual ~RedirectHandleable() {}

public:
    //! notifiy redirect started
    //! u're goning to receive redirect header and body
    virtual void OnRedirectingStarted() = 0;

    //! this will be called when the redirect header and body already received
    //! and about to perform the redirect
    virtual void OnRedirectingCompleted() = 0;

    //! return true if continue, otherwise will not perform redirection
    //! called after OnRedirectCompleted
    virtual bool WillRedirect(const Net::URL& location) = 0;
};

template<typename T>
class AsyncHandler : public CoreEventHandler
{
public:
    virtual ~AsyncHandler() {}

public:
    virtual T OnCompleted() = 0;
};


//! ###
//! TO-DO
//! alter the class name
//
class AsyncCompletionGenericDelegate
{
public:
    virtual ~AsyncCompletionGenericDelegate() {}
    virtual void OnRequestDataFilled() = 0;
    virtual void OnHeaderAvailable(const Net::HttpResponseHeaders& headers) = 0;
    virtual void OnBodyAvailable(InputStream& inputStream) = 0;

public:
    virtual void OnCompleted() = 0;
    virtual void OnError(Exception *ex) = 0;
    virtual void OnTerminated(){}
};

class RedirectCompletionGenericDelegate : public AsyncCompletionGenericDelegate
{
public:
    static RedirectCompletionGenericDelegate *GetDefaultDelegate();

private:
    CoreEventHandler *m_coreHandler;
    RedirectHandleable *m_redirectHandler;

    Net::URL m_locationURL;

public:
    RedirectCompletionGenericDelegate(CoreEventHandler *handler, RedirectHandleable *redirectHandler)
        : m_coreHandler(handler)
        , m_redirectHandler(redirectHandler)
        , m_locationURL()
    {}
    virtual ~RedirectCompletionGenericDelegate()
    {}

public:
    virtual void OnRequestDataFilled() { m_coreHandler->OnRequestDataFilled(); }
    virtual void OnHeaderAvailable(const Net::HttpResponseHeaders& headers) { m_coreHandler->OnHeaderAvailable(headers); }
    virtual void OnBodyAvailable(InputStream& inputStream){ m_coreHandler->OnBodyAvailable(inputStream); }

    virtual void OnCompleted()
    {
        m_redirectHandler->OnRedirectingCompleted();
    }
    virtual void OnError(Exception *ex)
    {
        Exception *after = m_coreHandler->OnException(ex);
    }

public:
    virtual void SetLocation(const Net::URL& url) { m_locationURL = url; }
    virtual bool PerformRedirecting() const { return m_redirectHandler->WillRedirect(m_locationURL); }
};

template<typename T>
class CompletionGenericDelegateImpl : public AsyncCompletionGenericDelegate
{
protected:
    AsyncHandler<T> *m_compltion;

public:
    CompletionGenericDelegateImpl(AsyncHandler<T> *handler)
        : m_compltion(handler)
    {}

    virtual ~CompletionGenericDelegateImpl()
    {}

public:
    virtual void OnRequestDataFilled() { m_compltion->OnRequestDataFilled(); }
    virtual void OnHeaderAvailable(const Net::HttpResponseHeaders& headers) { m_compltion->OnHeaderAvailable(headers); }
    virtual void OnBodyAvailable(InputStream& inputStream){ m_compltion->OnBodyAvailable(inputStream); }

    virtual void OnCompleted()
    {
        m_compltion->OnCompleted();
    }
    virtual void OnError(Exception *ex)
    {
        Exception *after = m_compltion->OnException(ex);
    }
};

template<typename T>
class  SyncCompletionGenericDelegate : public CompletionGenericDelegateImpl<T>
{
private:
    T *m_result;

public:
    SyncCompletionGenericDelegate(AsyncHandler<T> *handler, T *p)
        : CompletionGenericDelegateImpl<T>(handler)
        , m_result(p)
    {}

public:
    virtual void OnCompleted()
    {
        *m_result = m_compltion->OnCompleted();
    }
};

//! no f*cking alias
template<>
class SyncCompletionGenericDelegate<void> : public CompletionGenericDelegateImpl<void>
{
public:
    SyncCompletionGenericDelegate(AsyncHandler<void> *handler)
        : CompletionGenericDelegateImpl<void>(handler)
    {}
};

template<typename T>
class AsyncCompletionGenericDelegateImpl : public CompletionGenericDelegateImpl<T>
{
private:
    Promisee<T> m_promisee;

public:
    AsyncCompletionGenericDelegateImpl(AsyncHandler<T> *handler, const Promisee<T>& p)
        : CompletionGenericDelegateImpl<T>(handler)
        , m_promisee(p)
    {}

public:
    virtual void OnCompleted()
    {
        m_promisee.Resolve(m_compltion->OnCompleted());
    }

    virtual void OnError(Exception *ex)
    {
        m_promisee.Reject(m_compltion->OnException(ex));
    }
};

template<>
class AsyncCompletionGenericDelegateImpl<void> : public CompletionGenericDelegateImpl<void>
{
private:
    Promisee<void> m_promisee;

public:
    AsyncCompletionGenericDelegateImpl(AsyncHandler<void> *handler, const Promisee<void>& p)
        : CompletionGenericDelegateImpl<void>(handler)
        , m_promisee(p)
    {}

public:
    virtual void OnCompleted()
    {
        m_compltion->OnCompleted();
        m_promisee.Resolve();
    }

    virtual void OnError(Exception *ex)
    {
        m_promisee.Reject(m_compltion->OnException(ex));
    }
};

class IOException : public Exception
{
public:
    virtual std::string What() const { return "IOException"; }
    virtual Exception *Clone() const { return new IOException; }
};

template<typename T>
class AbstractDownloadAsyncHandler : public AsyncHandler<T>
{
protected:
    HANDLE m_hFile;

    std::wstring m_fileName;
    uint8_t *m_buffer;
    uint32_t m_bufferLength;

public:
    AbstractDownloadAsyncHandler(const std::wstring& file)
        : m_hFile(INVALID_HANDLE_VALUE)
        , m_fileName()
        , m_buffer(NULL)
        , m_bufferLength(4096)
    {
        m_hFile = ::CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

        if (INVALID_HANDLE_VALUE != m_hFile && NULL != m_hFile)
        {
            m_fileName = file;
            m_buffer = static_cast<uint8_t *>(::malloc(m_bufferLength));
        }
    }

    virtual ~AbstractDownloadAsyncHandler()
    {
        if (m_buffer)
            ::free(m_buffer);
    }

public:
    virtual void OnRequestDataFilled() {}

    virtual void OnBodyAvailable(InputStream& inputStream)
    {
        uint32_t alreadyRead = inputStream.Read(m_buffer, m_bufferLength);
        if (0 != alreadyRead)
        {
            DWORD dwReadSize = 0;
            if (!::WriteFile(m_hFile, m_buffer, alreadyRead, &dwReadSize, NULL) || dwReadSize != alreadyRead)
            {
                throw IOException();
            }
        }
    }
};

namespace Net
{
    class Part : public InputStream
    {
    public:
        virtual ~Part() {}

    public:
        virtual int64_t GetAvailCount() const = 0;
        virtual uint32_t Read(uint8_t *buffer, uint32_t read) = 0;
        virtual int64_t GetTotal() const = 0;

        virtual String GetPartHead() const = 0;
    };

    class SimpleFilePart : public Part
    {
    private:
        HANDLE m_hFile;
        String m_filePath;

    public:
        explicit SimpleFilePart(const String& path);
        virtual ~SimpleFilePart();

    public:
        virtual int64_t GetAvailCount() const = 0;
        virtual uint32_t Read(uint8_t *buffer, uint32_t read) = 0;
        virtual int64_t GetTotal() const = 0;
    };

    class NetException : public Exception
    {
    private:
        uint32_t m_errorCode;

    public:
        NetException(uint32_t code)
            : m_errorCode(code)
        {}

        virtual std::string What() const { return ""; }
        virtual Exception *Clone() const
        {
            return new NetException(m_errorCode);
        }
    };

    class InvalidURLFormatException : public Exception
    {
    public:
        virtual std::string What() const { return "InvalidURLFormatException"; }
        virtual Exception *Clone() const
        {
            return new InvalidURLFormatException;
        }
    };

    class ConnectionFailedException : public Exception
    {
    public:
        virtual std::string What() const { return "Connection failed"; }
        virtual Exception *Clone() const
        {
            return new ConnectionFailedException;
        }
    };

    class ConnectionTerminatedException : public Exception
    {
    public:
        virtual std::string What() const { return "Connection terminated"; }
        virtual Exception *Clone() const
        {
            return new ConnectionTerminatedException;
        }
    };

    template<typename ReturnType, bool takeOwnership>
    class HttpSyncTask : public Task<ReturnType>
    {
    private:
        AsyncHandler<ReturnType> *m_completion;
        HttpRequest *m_request;

    public:
        HttpSyncTask(HttpRequest *req, AsyncHandler<ReturnType> *completion)
            : m_completion(completion)
            , m_request(req)
        {}

    public:
        virtual ~HttpSyncTask() 
        {
            if (takeOwnership)
            {
                if (m_completion)
                    delete m_completion;

                if (m_request)
                    delete m_request;
            }
        }

    public:
        virtual ReturnType Run()
        {
            ReturnType result;

            SyncCompletionGenericDelegate<ReturnType> delegate(m_completion, &result);

            Net::HttpSessionConfig config;
            config.IsAsync = false;

            Net::HttpSession session(config);

            try
            {
                //! handler send request
                session.SendRequest(m_request, &delegate);
            }
            catch (const Exception& ex)
            {
                delegate.OnError(ex.Clone());
            }

            return result;
        }
    };

    template<bool takeOwnership>
    class HttpSyncTask<void, takeOwnership> : public Task<void>
    {
    private:
        AsyncHandler<void> *m_completion;
        HttpRequest *m_request;

    public:
        HttpSyncTask(HttpRequest *req, AsyncHandler<void> *completion)
            : m_completion(completion)
            , m_request(req)
        {}

    public:
        virtual ~HttpSyncTask()
        {
            if (takeOwnership)
            {
                if (m_completion)
                    delete m_completion;

                if (m_request)
                    delete m_request;
            }
        }

    public:
        virtual void Run()
        {
            SyncCompletionGenericDelegate<void> delegate(m_completion);

            Net::HttpSessionConfig config;
            config.IsAsync = false;

            Net::HttpSession session(config);

            try
            {
                //! handler send request
                session.SendRequest(m_request, &delegate);
            }
            catch (const Exception& ex)
            {
                delegate.OnError(ex.Clone());
            }
        }
    };

    template<typename ReturnType>
    class HttpAsyncTask : public AsyncTask<ReturnType>
    {
    private:
        AsyncHandler<ReturnType> *m_asyncHandler;
        HttpRequest *m_request;
        AsyncCompletionGenericDelegate *m_delegate;

    public:
        HttpAsyncTask(HttpRequest *req, AsyncHandler<ReturnType> *handler)
            : m_asyncHandler(handler)
            , m_request(req)
        {

        }

        ~HttpAsyncTask()
        {
            if (m_asyncHandler)
                delete m_asyncHandler;

            if (m_delegate)
                delete m_delegate;

            if (m_request)
                delete m_request;
        }

    public:
        virtual void OnEnter(ThreadLocalManager *tlm, const Promisee<ReturnType>& promisee)
        {
            //! get handler
            Net::HttpSession *session = NULL;
            session = tlm->Get<Net::HttpSession>();

            if (!session)
            {
                session = new Net::HttpSession;
                tlm->Register(session);
            }
            //
            //! handler send request
            m_delegate = new AsyncCompletionGenericDelegateImpl<ReturnType>(m_asyncHandler, promisee);

            try
            {
                session->SendRequest(m_request, m_delegate);
            }
            catch (const Exception& ex)
            {
                //! be aware
                m_delegate->OnError(ex.Clone());
            }
        }

        virtual void OnLeave(ThreadLocalManager *tlm)
        {
        }

        virtual void OnTerminated()
        {
            //! connection will be automatically disconnected
            //! do nothing
        }
    };

    //
    //  Http client supports async/sync operation in any context
    //      if the return type is kind of Promise, then current operation is asynchronous. However
    //      it doesn't mean the request sending and response receiving are also asynchronous. 
    //      if method name appended with 'Block' surfix, the request task will perform synchronously in target context. Otherwise, asynchronously.
    //
    //      if the return type is your handler completion type, the operation is typically synchronous
    //
    //      Imagine such situation:
    //          We'll fetch a captcha image from remote server and update UI label with that data
    //
    //      *** asynchronous operation, asynchronously invoking in thread pool context
    //      client.Get(L"http://localhost:8080/captcha/captcha.php", new CaptchaDownloadAsyncHandler, ThreadContext::FromThreadPool()).
    //                                                                                                         Then<void>(&this->m_captcha, ThreadContext::FromUIWindow(*this)).Done();
    //      
    //      *** asynchronous operation, synchronously invoking in worker thread context
    //      client.GetBlock(L"http://localhost:8080/captcha/captcha.php", new CaptchaDownloadAsyncHandler, myworker.Context()).
    //                                                                                                          Then<void>(&this->m_captcha, ThreadContext::FromUIWindow(*this)).Done();
    //
    //      *** synchronously operation, synchronously invoking in current context(UI context)
    //      CaptchaDownloadAsyncHandler handler;
    //      m_captcha.OnResult(client.Get(L"http://localhost:8080/captcha/captcha.php", &handler));
    //
    class HTTPCLIENT_EXPORT HttpClient
    {
    private:
        static AsyncHandler<HttpResponse> *AcquireDefaultHandler();

    public:
        template<typename T>
        Promise<T> Send(HttpRequest *request, AsyncHandler<T> *completion, const ThreadContext& context)
        {
            return Async::Make(new HttpAsyncTask<T>(request, completion), context);
        }

        template<typename T>
        Promise<T> Get(const URL& url, AsyncHandler<T> *completion, const ThreadContext& context)
        {
            return Async::Make(new HttpAsyncTask<T>(new HttpRequest(url), completion), context);
        }

        template<typename T>
        Promise<T> SendBlock(HttpRequest *request, AsyncHandler<T> *completion, const ThreadContext& context)
        {
            return Async::Make(new HttpSyncTask<T, true>(request, completion), context);
        }

        template<typename T>
        Promise<T> GetBlock(const URL& url, AsyncHandler<T> *completion, const ThreadContext& context)
        {
            return Async::Make(new HttpSyncTask<T, true>(new HttpRequest(url), completion), context);
        }

        /**
            Sync
            the ownership of request and async handler will NEVER be taken
            besides, exception instance with its ownership will be forward to your async handler

            exception
             may throw exception pointer
             u may catch the exception like this

             NB 
             if u won't catch it, it'll cause memory leak

             try
             {
                ... // ur code
             }
             catch (Exception *ex_ptr)
             {
                ... // do something
             }
         */
        template<typename T>
        T Send(HttpRequest *request, AsyncHandler<T> *completion)
        {
            HttpSyncTask<T, false> task(request, completion);
            return task.Run();
        }

        template<>
        void Send<void>(HttpRequest *request, AsyncHandler<void> *completion)
        {
            HttpSyncTask<void, false> task(request, completion);
            task.Run();
        }

        template<typename T>
        T Get(HttpRequest *request, AsyncHandler<T> *completion)
        {
            return Send(request, completion);
        }

        template<>
        void Get<void>(HttpRequest *request, AsyncHandler<void> *completion)
        {
           Send(request, completion);
        }

        template<typename T>
        T Get(const URL& url, AsyncHandler<T> *completion)
        {
            HttpRequest req(url);
            return Get(&req, completion);
        }

        template<>
        void Get<void>(const URL& url, AsyncHandler<void> *completion)
        {
            HttpRequest req(url);
            Get(&req, completion);
        }

        /**
            return HttpResponse
         */
        Promise<HttpResponse> Send(HttpRequest *request, const ThreadContext& context)
        {
            return Async::Make(new HttpAsyncTask<HttpResponse>(request, HttpClient::AcquireDefaultHandler()), context);
        }

        Promise<HttpResponse> Get(const URL& url, const ThreadContext& context)
        {
            return Async::Make(new HttpAsyncTask<HttpResponse>(new HttpRequest(url), HttpClient::AcquireDefaultHandler()), context);
        }

        Promise<HttpResponse> SendBlock(HttpRequest *request, const ThreadContext& context)
        {
            return Async::Make(new HttpSyncTask<HttpResponse, true>(request, HttpClient::AcquireDefaultHandler()), context);
        }

        Promise<HttpResponse> GetBlock(const URL& url, const ThreadContext& context)
        {
            return Async::Make(new HttpSyncTask<HttpResponse, true>(new HttpRequest(url), HttpClient::AcquireDefaultHandler()), context);
        }

        HttpResponse Get(const URL& url)
        {
            ScopedPointer<AsyncHandler<HttpResponse> > handler(HttpClient::AcquireDefaultHandler());
            return Get(url, handler.GetRaw());
        }

        HttpResponse Get(HttpRequest *request)
        {
            ScopedPointer<AsyncHandler<HttpResponse> > handler(HttpClient::AcquireDefaultHandler());
            return Get(request, handler.GetRaw());
        }

        HttpResponse Send(HttpRequest *request)
        {
            ScopedPointer<AsyncHandler<HttpResponse> > handler(HttpClient::AcquireDefaultHandler());
            return Send(request, handler.GetRaw());
        }
	};
}

#endif 