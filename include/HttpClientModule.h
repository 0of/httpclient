#ifndef HTTPCLIENTMODULE_H
#define HTTPCLIENTMODULE_H

#  pragma warning( push )
#  pragma warning( disable: 4251 )

#include "HttpClientExport.h"
#include "Thread.h"
#include "RefSharedPointer.h"

#include <string>
#include <map>
#include <cstdint>

typedef std::wstring String;

class AsyncCompletionGenericDelegate;

class HTTPCLIENT_EXPORT InputStream
{
public:
    virtual ~InputStream() {}

public:
    virtual int64_t GetAvailCount() const = 0;
    virtual uint32_t Read(uint8_t *buffer, uint32_t read) = 0;
    virtual int64_t GetTotal() const = 0;
};

namespace Net
{
    typedef String URL;

    enum HttpVerb
    {
        Get = 0, Post, Delete, Put
    };

    class HTTPCLIENT_EXPORT StatusCode
    {
    public:
        enum Value
        {
            Continue = 100,
            Switching_Protocols = 101,

            OK = 200,
            Created = 201,
            Accepted = 202,
            Non_Authoritative_Information = 203,
            No_Content = 204,
            Reset_Content = 205,
            Partial_Content = 206,

            Multiple_Choices = 300,
            Moved_Permanently = 301,
            Found = 302,
            See_Other = 303,
            Not_Modified = 304,
            Use_Proxy = 305,
            Temporary_Redirect = 307,

            Bad_Request = 400,
            Unauthorized = 401,
            Forbidden = 403,
            Not_Found = 404,
            Method_Not_Allowed = 405,
            Not_Acceptable = 406,
            Proxy_Authentication_Required = 407,
            Request_Timeout = 408,
            Conflict = 409,
            Gone = 410,
            Length_Required = 411,
            Precondition_Failed = 412,
            Request_Entity_Too_Large = 413,
            Request_URI_Too_Long = 414,
            Requested_Range_Not_Satisfiable = 416,

            Internal_Server_Error = 500,
            Not_Implemented = 501,
            Bad_Gateway = 502,
            Service_Unavailable = 503,
            Gateway_Timeout = 504,
            HTTP_Version_Not_Supported = 505
        };

    private:
        Value m_value;

    public:
        StatusCode(StatusCode::Value v)
            : m_value(v)
        {}

        StatusCode(const StatusCode& code)
            : m_value(code.m_value)
        {}

        ~StatusCode()
        {}

    public:
        bool operator == (StatusCode::Value v) { return m_value == v; }
        bool operator == (const StatusCode& code) { return m_value == code.m_value; }
        bool operator != (StatusCode::Value v) { return m_value != v; }
        bool operator != (const StatusCode& code) { return m_value != code.m_value; }

        StatusCode& operator = (StatusCode::Value v) { m_value = v; return *this; }
        StatusCode& operator = (const StatusCode& code) { m_value = code.m_value; return *this; }

    public:
        Value GetValue() const { return m_value; }
    };

    class HTTPCLIENT_EXPORT HttpResponseHeaders
    {
        friend class HttpResponse;

    private:
        class RawHeaders;

    private:
        StatusCode m_statusCode;

        RefSharedPointer<RawHeaders> m_rawHeaders;
        int64_t m_contentLength;

    public:
        HttpResponseHeaders();
        HttpResponseHeaders(const wchar_t *rawHeader);
        HttpResponseHeaders(const wchar_t *rawHeader, const StatusCode& statusCode, const int64_t& contentLength);
        HttpResponseHeaders(const HttpResponseHeaders&);

        ~HttpResponseHeaders();

        HttpResponseHeaders& operator = (const HttpResponseHeaders&);

    public:
        //!	no allocations, just headers offset
        const wchar_t *Get(const wchar_t *name, size_t& len) const;
        StatusCode GetStatusCode() const { return m_statusCode; }
        int64_t GetContentLength() const { return m_contentLength; }

        String GetHead(const wchar_t *headName) const;

    public:
        bool IsNull() const { return m_rawHeaders; }
    };

    class HTTPCLIENT_EXPORT HttpResponse
    {
    private:
        class HTTPCLIENT_EXPORT PrivateData;
        RefSharedPointer<PrivateData> m_d;

    public:
        HttpResponse();
        explicit HttpResponse(const HttpResponseHeaders& headers, InputStream *takenOwnershipStream);
        HttpResponse(const HttpResponse&);
        ~HttpResponse();

        HttpResponse& operator = (const HttpResponse&);

    public:
        InputStream *GetBodyStream() const;
        HttpResponseHeaders GetHeaders() const;

    public:
        bool IsNull() const { return NULL == m_d; }
    };

    class HTTPCLIENT_EXPORT RequestHeadersBuilder
    {
    private:
        typedef std::map<String, String> Headers;

    private:
        Headers m_headers;

    public:
        String ToString() const;

    public:
        RequestHeadersBuilder& operator << (const Headers::value_type& eachHeader);
    };

    class HTTPCLIENT_EXPORT HttpRequest
    {
    private:
        HttpVerb m_verb;

        URL m_url;
        String m_headersString;
        InputStream *m_requestBodyStream;

        //! cancellation token

    public:
        explicit HttpRequest(const URL& url, const HttpVerb& verb = Get)
            : m_verb(verb)
            , m_url(url)
            , m_headersString()
            , m_requestBodyStream(NULL)
        {}
        explicit HttpRequest(const URL& url, const HttpVerb& verb, const String& headersString, InputStream *bodyStream = NULL)
            : m_url(url)
            , m_headersString(headersString)
            , m_requestBodyStream(bodyStream)
            , m_verb(verb)
        {}
        ~HttpRequest() {}

    public:
        HttpVerb GetVerb() const { return m_verb; }
        const URL& GetURL() const { return m_url; }
        const String& GetHeadersString() const { return m_headersString; }
        InputStream *GetRequestBodyStream() const { return m_requestBodyStream; }

    public:
        bool HasHeaders() const { return !m_headersString.empty(); }

    private:
        HttpRequest(const HttpRequest&);
        HttpRequest& operator = (const HttpRequest&);
    };

    class HttpSessionConfig
    {
    public:
        //
        //  default is True
        //
        bool IsAsync;
        bool IsAutoRedirectEnabled;

    public:
        HttpSessionConfig()
            : IsAsync(true)
            , IsAutoRedirectEnabled(false)
        {}
    };

    class HTTPCLIENT_EXPORT HttpSession : public ThreadLocalModuleEBC<HttpSession>
    {
    public:
        class Private;

    private:
        Private *m_sessionImpl;

    public:
        //! use default config
        HttpSession();
        explicit HttpSession(const HttpSessionConfig& config);
        virtual ~HttpSession();

    public:
        //! do nothing
        virtual void OnUnregister() {}

    public:
        //! will block current thread
        void Disconnect();

    public:
        void SendRequest(const HttpRequest *req, AsyncCompletionGenericDelegate *delegate);
    };
}

#  pragma warning( pop )

#endif