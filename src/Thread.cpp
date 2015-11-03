#include "Thread.h"

ThreadContext::ThreadContext()
    : m_dwThreadID(0)
    , m_hHandle(NULL)
    , m_hWnd(NULL)
    , m_type(WorkerContext)
{

}

ThreadContext ThreadContext::Current()
{
    return ThreadContext(::GetCurrentThreadId(), ::GetCurrentThread());
}

ThreadContext ThreadContext::FromUIWindow(HWND hWnd)
{
    ThreadContext context;
    context.m_hWnd = hWnd;
    context.m_dwThreadID = ::GetWindowThreadProcessId(hWnd, NULL);
    context.m_type = ThreadContext::UIContext;

    return context;
}

ThreadContext ThreadContext::FromThreadPool()
{
    ThreadContext context;
    context.m_type = ThreadContext::ThreadPoolContext;

    return context;
}

ThreadContext::ThreadContext(DWORD dwThreaID, HANDLE hThread)
    : m_dwThreadID(dwThreaID)
    , m_hHandle(hThread)
    , m_hWnd(NULL)
{

}

ThreadContext::~ThreadContext()
{

}

bool ThreadContext::operator == (const ThreadContext& context) const
{
    return (m_type == ThreadContext::ThreadPoolContext && m_type == context.m_type) || m_dwThreadID == context.m_dwThreadID;
}

void ThreadLocalManager::Clean() throw()
{
    Objects::iterator it = m_objects.begin();
    for (; it != m_objects.end(); ++it)
    {
        it->second->OnUnregister();
        delete it->second;
    }

    m_objects.clear();
}

#define CALLABLE_MESSAGE		(WM_USER + 0x7000)
#define ASYNCALLABLE_MESSAGE	(WM_USER + 0x7002)
#define QUIT					(WM_USER + 0x7F00)

static DWORD WINAPI _ThreadPoolWorker(LPVOID lpThreadParameter)
{
    PMSG pMsg = (PMSG)lpThreadParameter;
    if (pMsg)
    {
        Dispatcher dispatcher;
        if (!dispatcher.EventDispatch(pMsg))
        {
            return (DWORD)-1;
        }
    }

    return (DWORD)-1;
}

namespace Private
{
    void _ThreadPoolAsyncGuard::OnEnter()
    {
        m_dwThreadID = ::GetCurrentThreadId();
    }

    void _ThreadPoolAsyncGuard::OnLeave()
    {
        ::PostThreadMessage(m_dwThreadID, QUIT, NULL, NULL);
    }
}

static DWORD WINAPI _ThreadPoolAsyncWorker(LPVOID lpThreadParameter)
{
    PMSG pMsg = (PMSG)lpThreadParameter;
    if (pMsg)
    {
        Dispatcher dispatcher;

        AsyncCallable *callable = (AsyncCallable *)pMsg->wParam;
        callable->OnEnter(&dispatcher);

        MSG msg;
        while (GetMessage(&msg, 0, WM_USER, 0))
        {
            if (!dispatcher.EventDispatch(&msg))
            {
                if (msg.message == QUIT)
                    return 0;
            }
        }
    }

    return -1;
}

void Dispatcher::PostCallable(Callable *callable, const ThreadContext& context)
{
    if (context.m_type == ThreadContext::ThreadPoolContext)
    {
        PMSG pMsg = (PMSG)::calloc(1, sizeof(MSG));
        if (pMsg)
        {
            pMsg->message = CALLABLE_MESSAGE;
            pMsg->wParam = (WPARAM)callable;

            ::QueueUserWorkItem(_ThreadPoolWorker, pMsg, 0);
        }
    }
    else if (context.m_hWnd != NULL)
    {
        ::PostMessage(context.m_hWnd, CALLABLE_MESSAGE, (WPARAM)callable, NULL);
    }
    else
    {
        ::PostThreadMessage(context.m_dwThreadID, CALLABLE_MESSAGE, (WPARAM)callable, NULL);
    }
}

void Dispatcher::PostCallable(AsyncCallable *callable, const ThreadContext& context)
{
    if (context.m_type == ThreadContext::ThreadPoolContext)
    {
        PMSG pMsg = (PMSG)::calloc(1, sizeof(MSG));
        if (pMsg)
        {
            pMsg->message = ASYNCALLABLE_MESSAGE;
            pMsg->wParam = (WPARAM)callable;

            ::QueueUserWorkItem(_ThreadPoolAsyncWorker, pMsg, 0);
        }
    }
    else if (context.m_hWnd != NULL)
    {
        ::PostMessage(context.m_hWnd, ASYNCALLABLE_MESSAGE, (WPARAM)callable, NULL);
    }
    else
    {
        ::PostThreadMessage(context.m_dwThreadID, ASYNCALLABLE_MESSAGE, (WPARAM)callable, NULL);
    }
}

template<typename T>
class _LocalPointer
{
    T *m_;
public:
    _LocalPointer(T *p) : m_(p){}
    ~_LocalPointer() { if (m_) delete m_; }

    T *operator -> () { return m_; }
};

bool Dispatcher::EventDispatch(PMSG pMsg)
{
    //! Window specific context
    if (pMsg->hwnd != NULL)
        return false;

    if (pMsg->message == CALLABLE_MESSAGE)
    {
        _LocalPointer<Callable> callable = (Callable *)pMsg->wParam;
        callable->Invoke();
        return true;
    }
    else if (pMsg->message == ASYNCALLABLE_MESSAGE)
    {
        AsyncCallable *callable = (AsyncCallable *)pMsg->wParam;
        callable->OnEnter(this);
    }

    return false;
}

bool Dispatcher::EventDispatch(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == CALLABLE_MESSAGE)
    {
        _LocalPointer<Callable> callable = (Callable *)wParam;
        callable->Invoke();
        return true;
    }
    else if (uMsg == ASYNCALLABLE_MESSAGE)
    {
        AsyncCallable *callable = (AsyncCallable *)wParam;
        callable->OnEnter(this);
    }

    return false;
}

void MessageLooper::Run()
{
    Dispatcher dispatcher;

    MSG msg;
    while (GetMessage(&msg, 0, 0, 0))
    {
        if (!dispatcher.EventDispatch(&msg))
        {
            if (msg.message == QUIT)
            {
                //!	notify
                return;
            }
        }
    }
}

void MessageLooper::Quit()
{
    ::PostMessage(NULL, QUIT, NULL, NULL);
}

static DWORD WINAPI WorkerProc(PVOID pParam)
{
    DWORD workerID = ::GetCurrentThreadId();

    if (pParam != NULL)
    {
        Callable *callable = (Callable *)pParam;
        callable->Invoke();
    }

    MessageLooper::Run();
    return 0;
}

Thread::Thread()
: m_context()
{

}

Thread::~Thread()
{
    Stop();
}

void Thread::Start(Callable *callable)
{
    m_context.m_hHandle = ::CreateThread(NULL, 0, WorkerProc, callable, 0, &m_context.m_dwThreadID);
}

void Thread::Stop()
{
    if (NULL != m_context.m_hHandle)
    {
        //!	send quit message 
        ::PostThreadMessage(m_context.m_dwThreadID, QUIT, NULL, NULL);

        DWORD ldwRst = ::WaitForSingleObject(m_context.m_hHandle, 5000);
        if (ldwRst == WAIT_TIMEOUT)
        {
            TerminateThread(m_context.m_hHandle, -1);
        }

        ::CloseHandle(m_context.m_hHandle);

        m_context.m_hHandle = NULL;
        m_context.m_dwThreadID = 0;
    }
}

#include <set>

DWORD ThreadLocalManager::Register(DWORD hint)
{
    typedef std::set<DWORD> Container;
    static Container ids;

    DWORD value = hint;

    if (ids.find(hint) != ids.end())
    {
        Container::const_iterator last = --ids.end();
        //! last one is the max one
        value = (*last) + 1;
    }

    ids.insert(value);

    return value;
}

