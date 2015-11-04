#include <iostream>

#include "HttpClient.h"
#include "Thread.h"

class GetContentLengthAsyncHandler : public AsyncHandler<std::int64_t>
{
private:
    std::int64_t m_len;

public:
    virtual void OnRequestDataFilled() {}
    virtual void OnHeaderAvailable(const Net::HttpResponseHeaders& headers) 
    {
        if (headers.GetStatusCode() != Net::StatusCode::OK) 
        {
            throw IOException();
        }

        m_len = headers.GetContentLength();
    }
    virtual void OnBodyAvailable(InputStream& inputStream)
    {
        // read some body data here
    }
    // return value must be same as inherited template parameter type
    virtual std::int64_t OnCompleted()
    {
        return m_len;
    }
    // filter any exceptions
    virtual Exception *OnException(Exception *ex) throw()
    {
        return ex;
    }
};

void OnContentLengthAcquired(std::int64_t contentLength)
{
    std::cout << "Content Length: " << contentLength << std::endl;
}

void OnException(const Exception& ex)
{
    std::cout << "Something goes wrong: " << ex.What() << std::endl;
}

int main() {
    // acquire an instance
    Net::HttpClient client;

    // run the http request task in thread pool
    // and print the content length in main thread context
    client.Get(L"http://localhost:8080/request.php", new GetContentLengthAsyncHandler, ThreadContext::FromThreadPool())
          .Then<void>(&OnContentLengthAcquired, &OnException, ThreadContext::Current())
          .Done();

    // run the message looper
    MessageLooper::Run();

    return 0;
}