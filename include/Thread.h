#ifndef THREAD_H
#define THREAD_H

#include <Windows.h>
#include <map>

#include "HttpClientExport.h"

///	thread related
class HTTPCLIENT_EXPORT ThreadContext
{
    friend class Dispatcher;
    friend class Thread;

    enum Type
    {
        UIContext,
        WorkerContext,
        ThreadPoolContext
    };

private:
    DWORD m_dwThreadID;

    HANDLE m_hHandle;
    HWND m_hWnd;

    Type m_type;

public:
    //! NB
    //! if u inside any thread and to get current context
    //! the type of context is always worker context
    static ThreadContext Current();
    static ThreadContext FromUIWindow(HWND hWnd);
    static ThreadContext FromThreadPool();

public:
    ThreadContext(DWORD dwThreaID, HANDLE hThread);
    ~ThreadContext();

public:
    bool IsThreadPool() const { return m_type == ThreadPoolContext; }

    DWORD GetThreadID() const { return m_dwThreadID; }

public:
    //! when thread pool context compares any other context, the result will be always false, unless both are thread pool context
    bool operator == (const ThreadContext& context) const;
    bool operator != (const ThreadContext& context) const
    {
        return !this->operator==(context);
    }

private:
    ThreadContext();
};

class HTTPCLIENT_EXPORT Callable
{
public:
    virtual ~Callable() {}
    virtual void Invoke() = 0;
};

class Dispatcher;
class HTTPCLIENT_EXPORT AsyncCallable
{
public:
    virtual ~AsyncCallable() {}
    virtual void OnEnter(Dispatcher *dispatcher) = 0;
    virtual void OnTerminated() = 0;
};

class HTTPCLIENT_EXPORT ThreadLocalModule
{
public:
    virtual ~ThreadLocalModule() {}
    virtual void OnUnregister() = 0;
};

#  pragma warning( push )
#  pragma warning( disable: 4251 )
// STL map is not safe export 
class HTTPCLIENT_EXPORT ThreadLocalManager
{
public:
    //! not thread safe
    static DWORD Register(DWORD hint = 0);

private:
    typedef std::map<DWORD, ThreadLocalModule *> Objects;
    Objects m_objects;

public:
    ~ThreadLocalManager()
    {
        Clean();
    }

public:
    template<typename T>
    bool Register(T *module)
    {
        if (m_objects.find(T::ID) != m_objects.end())
        {
            //! existed
            return false;
        }
        else
        {
            m_objects.insert(std::make_pair(T::ID, module));
            return true;
        }
    }

    template<typename T>
    T *Get()
    {
        Objects::iterator found = m_objects.find(T::ID);
        return found == m_objects.end() ? NULL : static_cast<T *>(found->second);
    }

    template<typename T>
    void Unregister()
    {
        m_objects.erase(T::ID);
    }

    //! delete all the modules
    void Clean() throw();
};
#  pragma warning( pop )


template<typename Subclass>
class ThreadLocalModuleEBC : public ThreadLocalModule
{
public:
    static DWORD ID;
};

template<typename Subclass>
DWORD ThreadLocalModuleEBC<Subclass>::ID = ThreadLocalManager::Register();

class HTTPCLIENT_EXPORT Dispatcher
{
    ThreadLocalManager m_manager;

public:
    static void PostCallable(Callable *callable, const ThreadContext& context);
    static void PostCallable(AsyncCallable *callable, const ThreadContext& context);

public:
    bool EventDispatch(PMSG pMsg);
    bool EventDispatch(UINT uMsg, WPARAM wParam, LPARAM lParam);
    ThreadLocalManager *GetThreadLocalManager() { return &m_manager; }
};

class HTTPCLIENT_EXPORT MessageLooper
{
public:
    //! includes Dispatcher
    static void Run();
    static void Quit();
};

namespace Private
{
    class HTTPCLIENT_EXPORT _ThreadPoolAsyncGuard
    {
    private:
        DWORD m_dwThreadID;

    public:
        void OnEnter();
        void OnLeave();
    };
}

#ifdef _WIN64
#define REF volatile LONGLONG
#define Increment InterlockedIncrement64
#define Decrement InterlockedDecrement64
#define CompareExchange InterlockedCompareExchange64
#else
#define REF volatile LONG
#define Increment InterlockedIncrement
#define Decrement InterlockedDecrement
#define CompareExchange InterlockedCompareExchange
#endif

class HTTPCLIENT_EXPORT AtomicRef
{
private:
    REF m_ref;

public:
    AtomicRef()
        : m_ref(1)
    {}

    ~AtomicRef()
    {}

public:
    void AddRef() { ::Increment(&m_ref); }
    bool Release()
    {
        return 0 == ::Decrement(&m_ref);
    }
};

class CriticalSection
{
    CRITICAL_SECTION m_criticalSection;

public:
    CriticalSection()
        : m_criticalSection()
    {
        ::InitializeCriticalSection(&m_criticalSection);
    }
    ~CriticalSection()
    {
        ::DeleteCriticalSection(&m_criticalSection);
    }

public:
    void Lock()
    {
        ::EnterCriticalSection(&m_criticalSection);
    }
    void Unlock()
    {
        ::LeaveCriticalSection(&m_criticalSection);
    }
};

class ManualResetEvent
{
private:
    HANDLE m_resetEvent;

public:
    ManualResetEvent()
        : m_resetEvent(NULL)
    {
        m_resetEvent = ::CreateEvent(NULL, TRUE, FALSE, NULL);
    }

    explicit ManualResetEvent(bool isSignaled)
        : m_resetEvent(NULL)
    {
        m_resetEvent = ::CreateEvent(NULL, TRUE, isSignaled, NULL);
    }
    ~ManualResetEvent()
    {
        if (m_resetEvent)
            ::CloseHandle(m_resetEvent);
    }

public:
    bool Wait(DWORD timeout)
    {
        return WAIT_OBJECT_0 == ::WaitForSingleObject(m_resetEvent, timeout);
    }

    bool IsSignaled() const
    {
        return WAIT_OBJECT_0 == ::WaitForSingleObject(m_resetEvent, 0);
    }

    void Signal()
    {
        ::SetEvent(m_resetEvent);
    }

    void Reset()
    {
        ::ResetEvent(m_resetEvent);
    }
};

template<typename T>
class AutoLock
{
private:
    T *m_locker;

public:
    AutoLock(T *locker)
        : m_locker(locker)
    {
        m_locker->Lock();
    }

    ~AutoLock()
    {
        m_locker->Unlock();
    }
};

class HTTPCLIENT_EXPORT Thread
{
private:
    ThreadContext m_context;

public:
    Thread();
    ~Thread();

public:
    void Start(Callable *callable = NULL);
    void Stop();

public:
    ThreadContext Context() const { return m_context; }
};

#endif