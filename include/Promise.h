#ifndef PROMISE_H
#define PROMISE_H

#include <string>

#include "Thread.h"
#include "Traits.h"
#include "ScopedPointer.h"

class Exception
{
public:
    virtual ~Exception() {}
    virtual std::string What() const = 0;
    virtual Exception *Clone() const = 0;
};

///////////////////////////////////////
//
//
//	Promise provider
//	NOT thread safe
//
//	When promise something, it must be allocated locally
//	When promise context is not equal to current, any copy will throw exception
//
//	\tparam T return type
//
template<typename T>
class Promisee;

//
//	Promise
//
//	\tparam T return type
//
template<typename T>
class Promise;


//
//
template<typename ReturnType>
class Task
{
public:
    virtual ~Task() {}
    virtual ReturnType Run() = 0;
};

//
//
//
template<typename ReturnType>
class AsyncTask
{
public:
    virtual ~AsyncTask() {}
    virtual void OnEnter(ThreadLocalManager *tlm, const Promisee<ReturnType>& promisee) = 0;
    virtual void OnLeave(ThreadLocalManager *tlm) = 0;
    virtual void OnTerminated() {}
};


namespace Detail
{
    namespace Core
    {
        enum ProcState
        {
            Ready,
            Running,
            ExceptionOccurred,
            Cancelled,
            Finished
        };

        template<typename T>
        class PromiseProc
        {
        public:
            virtual ~PromiseProc() {}
            virtual T Run() = 0;
        };

        template<typename T>
        class PromiseForwarder
        {
        public:
            virtual ~PromiseForwarder() {}
            virtual void Forward(const T& value) = 0;
            virtual void ForwardException(Exception *ex) = 0;
        };

        template<>
        class PromiseForwarder<void>
        {
        public:
            virtual ~PromiseForwarder() {}
            virtual void ForwardException(Exception *ex) = 0;
        };

        template<typename Arg, typename Return>
        class PromiseAbstractProc : public PromiseForwarder<Arg>, public PromiseProc<Return>
        {
        protected:
            Exception *m_exception;

        protected:
            PromiseAbstractProc()
                : m_exception(NULL)
            {}

        public:
            virtual ~PromiseAbstractProc()
            {}

        public:
            virtual void ForwardException(Exception *ex)
            {
                m_exception = ex;
            }
        };

        template<typename T>
        class PromiseNext
        {
        public:
            virtual ~PromiseNext() {}
            virtual void OnFulfill(const T& value) throw() = 0;
            virtual void OnRefused(Exception *exception) throw() = 0;
        };

        template<>
        class PromiseNext<void>
        {
        public:
            virtual ~PromiseNext() {}
            virtual void OnFulfill() throw() = 0;
            virtual void OnRefused(Exception *exception) throw() = 0;
        };

        template<typename ReturnType>
        struct RunTrait
        {
            template<template<typename Type> class Proc>
            static void OnRun(Proc<ReturnType> *proc, PromiseNext<ReturnType> *next)
            {
                if (next)
                {
                    try
                    {
                        next->OnFulfill(proc->Run());
                    }
                    catch (const Exception& ex)
                    {
                        next->OnRefused(ex.Clone());
                    }
                    catch (Exception *ex_ptr)
                    {
                        next->OnRefused(ex_ptr);
                    }
                }
                else
                {
                    (void)proc->Run();	//!	ignore return value
                }
            }
        };

        template<>
        struct RunTrait<void>
        {
            template<template<typename Type> class Proc>
            static void OnRun(Proc<void> *proc, PromiseNext<void> *next)
            {
                if (next)
                {
                    try
                    {
                        proc->Run();
                        next->OnFulfill();
                    }
                    catch (const Exception& ex)
                    {
                        next->OnRefused(ex.Clone());
                    }
                    catch (Exception *ex_ptr)
                    {
                        next->OnRefused(ex_ptr);
                    }
                }
                else
                {
                    proc->Run(); //! ignore return value
                }
            }
        };

        class PromiseChainHead
        {
        public:
            virtual void AddRef() = 0;
            virtual void Release() = 0;

        public:
            virtual void Start() = 0;
        };

        template<typename T>
        class PromiseCore : public PromiseChainHead
        {
            template<typename T>
            friend class PromiseCore;

        public:
            //!	for task
            template<template<typename ArgType, typename ReturnType> class CreateTrait, typename Task>
            static typename std::enable_if<CreateTrait<void, T>::isValid, PromiseCore<T> *>::type Create(Task *any, const ThreadContext& context)
            {
                PromiseCore<T> *core = new PromiseCore<T>;
                PromiseProc<T> *proc = CreateTrait<void, T>::CreateProc(any, core);

                core->m_proc = proc;
                core->m_runningContext = context;

                return core;
            }

        private:
            PromiseProc<T> *m_proc;
            PromiseNext<T> *m_next;

            ThreadContext m_runningContext;
            AtomicRef m_ref;

            PromiseChainHead *m_head;

        private:
            PromiseCore()
                : m_proc(NULL)
                , m_next(NULL)
                , m_runningContext(ThreadContext::Current())
                , m_ref()
                , m_head(NULL)
            {

            }

            ~PromiseCore()
            {
                if (m_proc)
                    delete m_proc;

                if (m_next)
                    delete m_next;

                if (m_head)
                    m_head->Release();
            }

        public:
            virtual void AddRef()
            {
                m_ref.AddRef();
            }

            virtual void Release()
            {
                if (m_ref.Release())
                    delete this;
            }

            virtual void Start()
            {
                if (m_head)
                    throw std::logic_error("Not promise chain head");

                //!	ignored return type
                (void)m_proc->Run();
            }

            void Run()
            {
                if (!m_runningContext.IsThreadPool() && m_runningContext != ThreadContext::Current())
                {
                    throw std::logic_error("Context conflicts");
                }

                RunTrait<T>::OnRun(m_proc, m_next);
            }

            void Clean()
            {
                if (m_proc)
                {
                    delete m_proc;
                    m_proc = NULL;
                }

                if (m_next)
                {
                    delete m_next;
                    m_next = NULL;
                }

                Release();
            }

            template<typename NextReturnType, template<typename ArgType, typename ReturnType> class AppendTrait, typename AnyType>
            typename std::enable_if<AppendTrait<T, NextReturnType>::isValid, PromiseCore<NextReturnType> *>::type AppendNext(AnyType any, const ThreadContext& context) throw()	//!	noexcept
            {
                //!	next proc
                PromiseAbstractProc<T, NextReturnType> *proc = AppendTrait<T, NextReturnType>::CreateProc(any);

                //!	create next core
                PromiseCore<NextReturnType> *nextCore = new PromiseCore<NextReturnType>;
                nextCore->m_proc = proc;
                nextCore->m_runningContext = context;

                if (m_head)
                {
                    m_head->AddRef();
                    nextCore->m_head = m_head;
                }
                else
                {
                    AddRef();
                    nextCore->m_head = this;
                }

                //!	create next 
                this->m_next = AppendTrait<T, NextReturnType>::CreateNext(nextCore, proc);

                return nextCore;
            }

        public:
            PromiseNext<T> *GetNext() const
            {
                return m_next;
            }

            PromiseChainHead *GetHead()
            {
                return m_head ? m_head : this;
            }

            ThreadContext GetContext() const { return m_runningContext; }
        };
    }

    namespace GeneralImpl
    {
        template<typename T, typename NextReturnType>
        class PromiseCallable : public Callable
        {
        private:
            Detail::Core::PromiseCore<NextReturnType> *m_nextCore;
            Detail::Core::PromiseForwarder<T> *m_forwarder;
            T m_promiseValue;

        public:
            PromiseCallable(Detail::Core::PromiseForwarder<T> *forwarder, const T& v, Detail::Core::PromiseCore<NextReturnType> *core)
                : m_forwarder(forwarder)
                , m_nextCore(core)
                , m_promiseValue(v)
            {
                m_nextCore->AddRef();
            }

            ~PromiseCallable()
            {
                m_nextCore->Clean();
            }

        public:
            virtual void Invoke()
            {
                m_forwarder->Forward(m_promiseValue);
                m_nextCore->Run();
            }
        };

        template<typename NextReturnType>
        class PromiseCallable<void, NextReturnType> : public Callable
        {
        private:
            Detail::Core::PromiseCore<NextReturnType> *m_nextCore;

        public:
            PromiseCallable(Detail::Core::PromiseCore<NextReturnType> *core)
                : m_nextCore(core)
            {
                m_nextCore->AddRef();
            }

            ~PromiseCallable()
            {
                m_nextCore->Clean();
            }

        public:
            virtual void Invoke()
            {
                m_nextCore->Run();
            }
        };

        template<typename T, typename NextReturnType>
        class ExceptionCallable : public Callable
        {
        private:
            Detail::Core::PromiseCore<NextReturnType> *m_nextCore;
            Detail::Core::PromiseForwarder<T> *m_forwarder;
            Exception *m_exception;

        public:
            ExceptionCallable(Detail::Core::PromiseForwarder<T> *forwarder, Exception *ex, Detail::Core::PromiseCore<NextReturnType> *core)
                : m_nextCore(core)
                , m_forwarder(forwarder)
                , m_exception(ex)
            {
                m_nextCore->AddRef();
            }

            ~ExceptionCallable()
            {
                m_nextCore->Clean();
            }

        public:
            virtual void Invoke()
            {
                m_forwarder->ForwardException(m_exception);
                m_nextCore->Run();
            }
        };

        template<typename ArgType, typename NextReturnType>
        class GenericPromiseNext : public Detail::Core::PromiseNext<ArgType>
        {
        private:
            Detail::Core::PromiseCore<NextReturnType> *m_nextCore;
            Detail::Core::PromiseForwarder<ArgType> *m_forwarder;

        public:
            GenericPromiseNext(Detail::Core::PromiseCore<NextReturnType> *core, Detail::Core::PromiseForwarder<ArgType> *forwarder)
                : m_nextCore(core)
                , m_forwarder(forwarder)
            {
                m_nextCore->AddRef();
            }

            ~GenericPromiseNext()
            {
                m_nextCore->Release();
            }

        public:
            virtual void OnFulfill(const ArgType& value)
            {
                if (m_nextCore->GetContext() == ThreadContext::Current())
                {
                    PromiseCallable<ArgType, NextReturnType> callable(m_forwarder, value, m_nextCore);
                    callable.Invoke();
                }
                else
                {
                    Dispatcher::PostCallable(new PromiseCallable<ArgType, NextReturnType>(m_forwarder, value, m_nextCore), m_nextCore->GetContext());
                }
            }

            virtual void OnRefused(Exception *exception)
            {
                Dispatcher::PostCallable(new ExceptionCallable<ArgType, NextReturnType>(m_forwarder, exception, m_nextCore), m_nextCore->GetContext());
            }
        };

        template<typename NextReturnType>
        class GenericPromiseNext<void, NextReturnType> : public Detail::Core::PromiseNext<void>
        {
        private:
            //!	can be NULL
            Detail::Core::PromiseCore<NextReturnType> *m_nextCore;
            Detail::Core::PromiseForwarder<void> *m_forwarder;

        public:
            GenericPromiseNext(Detail::Core::PromiseCore<NextReturnType> *core, Detail::Core::PromiseForwarder<void> *forwarder)
                : m_nextCore(core)
                , m_forwarder(forwarder)
            {
                m_nextCore->AddRef();
            }

            ~GenericPromiseNext()
            {
                m_nextCore->Release();
            }

        public:
            virtual void OnFulfill()
            {
                if (m_nextCore->GetContext() == ThreadContext::Current())
                {
                    PromiseCallable<void, NextReturnType> callable(m_nextCore);
                    callable.Invoke();
                }
                else
                {
                    Dispatcher::PostCallable(new PromiseCallable<void, NextReturnType>(m_nextCore), m_nextCore->GetContext());
                }
            }

            virtual void OnRefused(Exception *exception)
            {
                Dispatcher::PostCallable(new ExceptionCallable<void, NextReturnType>(m_forwarder, exception, m_nextCore), m_nextCore->GetContext());
            }
        };

        namespace StaticInheritedClassImpl
        {
            template<typename StaticInheritedClass, typename ArgType, typename ReturnType>
            class StaticInheritedClassPromiseProc : public Detail::Core::PromiseAbstractProc<ArgType, ReturnType>
            {
            private:
                StaticInheritedClass *m_object;
                ArgType m_arg;

            public:
                StaticInheritedClassPromiseProc(StaticInheritedClass *obj)
                    : m_object(obj)
                {

                }

            public:
                virtual ReturnType Run()
                {
                    if (m_exception)
                    {
                        ScopedPointer<Exception> ex(m_exception);
                        return m_object->OnException(*ex);
                    }
                    else
                    {
                        return m_object->OnResult(m_arg);
                    }
                }

            public:
                virtual void Forward(const ArgType& value)
                {
                    m_arg = value;
                }
            };

            template<typename StaticInheritedClass, typename ReturnType>
            class StaticInheritedClassPromiseProc<StaticInheritedClass, void, ReturnType> : public Detail::Core::PromiseAbstractProc<void, ReturnType>
            {
            private:
                StaticInheritedClass *m_object;

            public:
                StaticInheritedClassPromiseProc(StaticInheritedClass *obj)
                    : m_object(obj)
                {

                }

            public:
                virtual ReturnType Run()
                {
                    if (m_exception)
                    {
                        ScopedPointer<Exception> ex(m_exception);
                        return m_object->OnException(*ex);
                    }
                    else
                    {
                        m_object->OnResult();
                    }
                }
            };

            template<typename ArgType, typename ReturnType>
            struct StaticInheritedClassAppendTrait
            {
                template<typename StaticInheritedClass>
                static Detail::Core::PromiseAbstractProc<ArgType, ReturnType> *CreateProc(StaticInheritedClass *any)
                {
                    return new StaticInheritedClassPromiseProc<StaticInheritedClass, ArgType, ReturnType>(any);
                }

                static Detail::Core::PromiseNext<ArgType> *CreateNext(Detail::Core::PromiseCore<ReturnType> * nextCore, Detail::Core::PromiseForwarder<ArgType> *forwarder)
                {
                    return new GenericPromiseNext<ArgType, ReturnType>(nextCore, forwarder);
                }

                //!	cpp 11
                /*	enum
                {
                isValid = CanConvert<decltype(Arg0Trait::Type(&NextType::OnResult)), ArgValue>::value &&
                CanConvert<decltype(ReturnTrait::Type(&NextType::OnResult)), ReturnValue>::value
                };*/

                enum { isValid = 1 };
            };
        }

        namespace FunctorImpl
        {
            template<typename ArgType, typename ReturnType>
            struct Functor
            {
                typedef ReturnType(*OnSuccess)(ArgType);
                typedef ReturnType(*OnException)(const Exception&);

                OnSuccess m_onSuccess;
                OnException m_onException;
            };

            template<typename ArgType, typename ReturnType>
            class FunctorPromiseProc : public Detail::Core::PromiseAbstractProc<ArgType, ReturnType>
            {
            private:
                Functor<ArgType, ReturnType> m_functor;
                ArgType m_arg;

            public:
                FunctorPromiseProc(const Functor<ArgType, ReturnType> &functor)
                    : m_functor(functor)
                    , m_arg()
                {

                }

            public:
                virtual ReturnType Run()
                {
                    if (m_exception)
                    {
                        ScopedPointer<Exception> ex(m_exception);
                        return m_functor.m_onException(*ex);
                    }
                    else
                    {
                        return m_functor.m_onSuccess(m_arg);
                    }
                }

            public:
                virtual void Forward(const ArgType& value)
                {
                    m_arg = value;
                }
            };

            template<typename ReturnType>
            class FunctorPromiseProc<void, ReturnType> : public Detail::Core::PromiseAbstractProc<void, ReturnType>
            {
            private:
                Functor <void, ReturnType> m_functor;

            public:
                FunctorPromiseProc(const Functor<void, ReturnType> &functor)
                    : m_functor(functor)
                {

                }

            public:
                virtual ReturnType Run()
                {
                    if (m_exception)
                    {
                        ScopedPointer<Exception> ex(m_exception);
                        return m_functor.m_onException(*ex);
                    }
                    else
                    {
                        return m_functor.m_onSuccess();
                    }
                }
            };

            template<typename ArgType, typename ReturnType>
            struct FunctorAppendTrait
            {
                static Detail::Core::PromiseAbstractProc<ArgType, ReturnType> *CreateProc(const Functor<ArgType, ReturnType> &functor)
                {
                    return new FunctorPromiseProc<ArgType, ReturnType>(functor);
                }

                static Detail::Core::PromiseNext<ArgType> *CreateNext(Detail::Core::PromiseCore<ReturnType> * nextCore, Detail::Core::PromiseForwarder<ArgType> *forwarder)
                {
                    return new GenericPromiseNext<ArgType, ReturnType>(nextCore, forwarder);
                }

                enum { isValid = 1 };
            };
        }
    }


    namespace TaskBased
    {
        template<typename ReturnType>
        class AsyncTaskWrapper : public AsyncCallable
        {
        private:
            Dispatcher *m_dispatcher;

            AsyncTask<ReturnType> *m_task;
            Detail::Core::PromiseCore<ReturnType> *m_core;

        public:
            AsyncTaskWrapper(AsyncTask<ReturnType> *task, Detail::Core::PromiseCore<ReturnType> *core)
                : m_task(task)
                , m_core(core)
            {
                m_core->AddRef();
            }
            virtual ~AsyncTaskWrapper()
            {
                delete m_task;
                m_core->Clean();
            }

        public:
            void OnEnter(Dispatcher *dispatcher)
            {
                //!	register self

                m_dispatcher = dispatcher;

                Promisee<ReturnType> promisee(m_core, this);
                m_task->OnEnter(dispatcher->GetThreadLocalManager(), promisee);
            }

            void OnTerminated()
            {
                m_task->OnTerminated();
                //!	
            }

            void OnCleanup()
            {
                //!	will delete self
                ScopedPointer<AsyncTaskWrapper<ReturnType> > autoself(this);
                m_task->OnLeave(m_dispatcher->GetThreadLocalManager());
            }
        };

        template<typename ReturnType>
        class TaskCallable : public Callable
        {
        private:
            Task<ReturnType> *m_task;
            Detail::Core::PromiseCore<ReturnType> *m_currentCore;

        public:
            TaskCallable(Task<ReturnType> *task, Detail::Core::PromiseCore<ReturnType> *currentCore)
                : m_task(task)
                , m_currentCore(currentCore)
            {
                m_currentCore->AddRef();
            }

            virtual ~TaskCallable()
            {
                m_currentCore->Clean();
            }

        public:
            virtual void Invoke()
            {
                Detail::Core::RunTrait<ReturnType>::OnRun(m_task, m_currentCore->GetNext());
            }
        };

        template<typename ReturnType>
        class TaskPromiseProc : public Detail::Core::PromiseProc<ReturnType>
        {
        private:
            Task<ReturnType> *m_task;
            Detail::Core::PromiseCore<ReturnType> *m_core;

        public:
            TaskPromiseProc(Task<ReturnType> *task, Detail::Core::PromiseCore<ReturnType> *core)
                : m_task(task)
                , m_core(core)
            {}

        public:
            virtual ReturnType Run()
            {
                if (m_core->GetContext() == ThreadContext::Current())
                {
                    TaskCallable<ReturnType> callable(m_task, m_core);
                    callable.Invoke();
                }
                else
                {
                    Dispatcher::PostCallable(new TaskCallable<ReturnType>(m_task, m_core), m_core->GetContext());
                }
                return ReturnType();	//!	fake
            }
        };

        template<typename ReturnType>
        class ThreadPoolAsyncTask : public Private::_ThreadPoolAsyncGuard, public AsyncTask<ReturnType>
        {
        private:
            AsyncTask<ReturnType> *m_task;

        public:
            ThreadPoolAsyncTask(AsyncTask<ReturnType> *task)
                : m_task(task)
            {}

            virtual ~ThreadPoolAsyncTask()
            {
                delete m_task;
            }

        public:
            virtual void OnEnter(ThreadLocalManager *tlm, const Promisee<ReturnType>& promisee)
            {
                Private::_ThreadPoolAsyncGuard::OnEnter();
                m_task->OnEnter(tlm, promisee);
            }
            virtual void OnLeave(ThreadLocalManager *tlm)
            {
                m_task->OnLeave(tlm);
                Private::_ThreadPoolAsyncGuard::OnLeave();
            }
        };

        template<typename ReturnType>
        class AsyncTaskPromiseProc : public Detail::Core::PromiseProc<ReturnType>
        {
        private:
            AsyncTask<ReturnType> *m_task;
            Detail::Core::PromiseCore<ReturnType> *m_core;

        public:
            AsyncTaskPromiseProc(AsyncTask<ReturnType> *task, Detail::Core::PromiseCore<ReturnType> *core)
                : m_task(task)
                , m_core(core)
            {}

        public:
            virtual ReturnType Run()
            {
                const ThreadContext& context = m_core->GetContext();
                AsyncCallable *callable = context.IsThreadPool() ? new AsyncTaskWrapper<ReturnType>(new ThreadPoolAsyncTask<ReturnType>(m_task), m_core)
                    : new AsyncTaskWrapper<ReturnType>(m_task, m_core);

                Dispatcher::PostCallable(callable, context);
                return ReturnType();	//!	fake
            }
        };


        template<typename ArgType, typename ReturnType>
        struct TaskCreateTrait
        {
            static Detail::Core::PromiseProc<ReturnType> *CreateProc(Task<ReturnType> *any, Detail::Core::PromiseCore<ReturnType> *core)
            {
                return new TaskPromiseProc<ReturnType>(any, core);
            }

            enum { isValid = 1 };
        };

        template<typename ArgType, typename ReturnType>
        struct AsyncTaskCreateTrait
        {
            static Detail::Core::PromiseProc<ReturnType> *CreateProc(AsyncTask<ReturnType> *any, Detail::Core::PromiseCore<ReturnType> *core)
            {
                return new AsyncTaskPromiseProc<ReturnType>(any, core);
            }

            enum { isValid = 1 };
        };
    }
}


////////////////////////////////////////////////////////////////////
//	Implementations
//

//	provider
//
template<typename T>
class Promisee
{
    friend class Async;

private:
    //	registered by async methods
    Detail::Core::PromiseCore<T> *m_core;
    Detail::TaskBased::AsyncTaskWrapper<T> *m_wrapper;

public:
    Promisee()
        : m_core(NULL)
        , m_wrapper(NULL)
    {}

    Promisee(Detail::Core::PromiseCore<T> *core, Detail::TaskBased::AsyncTaskWrapper<T> *wrapper)
        : m_core(core)
        , m_wrapper(wrapper)
    {
        core->AddRef();
    }

    Promisee(const Promisee<T>& r)
        : m_core(r.m_core)
        , m_wrapper(r.m_wrapper)
    {
        m_core->AddRef();
    }

    ~Promisee()
    {
        m_core->Release();
    }

    Promisee& operator = (const Promisee<T>& r)
    {
        if (m_core == NULL && m_core != r.m_core)
        {
            m_core = r.m_core;
            m_wrapper = r.m_wrapper;

            m_core->AddRef();
        }

        return *this;
    }

public:
    //
    //  when resolved, the referenced context may be deleted
    //
    void Resolve(const T& value) const
    {
        m_core->GetNext()->OnFulfill(value);
        m_wrapper->OnCleanup();
    }

    void Reject(Exception *e) const
    {
        m_core->GetNext()->OnRefused(e);
        m_wrapper->OnCleanup();
    }
};

template<>
class Promisee<void>
{
    friend class Async;
    friend class Promisee<void>;

private:
    //	registered by async methods
    Detail::Core::PromiseCore<void> *m_core;
    Detail::TaskBased::AsyncTaskWrapper<void> *m_wrapper;

public:
    Promisee()
        : m_core(NULL)
        , m_wrapper(NULL)
    {}

    Promisee(Detail::Core::PromiseCore<void> *core, Detail::TaskBased::AsyncTaskWrapper<void> *wrapper)
        : m_core(core)
        , m_wrapper(wrapper)
    {
        core->AddRef();
    }

    Promisee(const Promisee<void>& r)
        : m_core(r.m_core)
        , m_wrapper(r.m_wrapper)
    {
        m_core->AddRef();
    }

    ~Promisee()
    {
        m_core->Release();
    }

    Promisee& operator = (const Promisee<void>& r)
    {
        if (m_core == NULL && m_core != r.m_core)
        {
            m_core = r.m_core;
            m_wrapper = r.m_wrapper;

            m_core->AddRef();
        }

        return *this;
    }

public:
    //
    //  when resolved, the referenced context may be deleted
    //
    void Resolve() const
    {
        m_core->GetNext()->OnFulfill();
        m_wrapper->OnCleanup();
    }

    void Reject(Exception *e) const
    {
        m_core->GetNext()->OnRefused(e);
        m_wrapper->OnCleanup();
    }
};

class Async
{
public:
    //!	Async
    template<typename T>
    static Promise<T> Make(Task<T> *task, const ThreadContext& context)
    {
        Detail::Core::PromiseCore<T> *core = Detail::Core::PromiseCore<T>::Create<Detail::TaskBased::TaskCreateTrait>(task, context);

        Promise<T> promise;
        promise.m_core = core;

        return promise;
    }

    template<typename T>
    static Promise<T> Make(AsyncTask<T> *task, const ThreadContext& context)
    {
        Detail::Core::PromiseCore<T> *core = Detail::Core::PromiseCore<T>::Create<Detail::TaskBased::AsyncTaskCreateTrait>(task, context);

        Promise<T> promise;
        promise.m_core = core;

        return promise;
    }
};

template<typename T>
class Promise
{
    template<typename T>
    friend class Promise;

    friend class Promisee<T>;

    friend class Async;

private:
    Detail::Core::PromiseCore<T> *m_core;

public:
    template<typename Return, typename StaticInheritedClass>
    Promise<Return> Then(StaticInheritedClass *object, const ThreadContext& context)
    {
        Detail::Core::PromiseCore<Return> *nextCore = m_core->AppendNext<Return, Detail::GeneralImpl::StaticInheritedClassImpl::StaticInheritedClassAppendTrait>(object, context);

        Promise<Return> nextPromise;
        nextPromise.m_core = nextCore;

        return nextPromise;
    }

    template<typename Return>
    Promise<Return> Then(Return(*OnSuccess)(T), Return(*OnException)(const Exception&), const ThreadContext& context)
    {
        Detail::GeneralImpl::FunctorImpl::Functor<T, Return> functor;
        functor.m_onException = OnException;
        functor.m_onSuccess = OnSuccess;
        Detail::Core::PromiseCore<Return> *nextCore = m_core->AppendNext<Return, Detail::GeneralImpl::FunctorImpl::FunctorAppendTrait>(functor, context);

        Promise<Return> nextPromise;
        nextPromise.m_core = nextCore;

        return nextPromise;
    }

public:
    void Done()
    {
        m_core->GetHead()->Start();
    }

private:
    Promise()
        : m_core(NULL)
    {}

public:
    Promise(const Promise<T>& promise)
        : m_core(promise.m_core)
    {
        if (m_core)
            m_core->AddRef();
    }

    ~Promise()
    {
        if (m_core)
            m_core->Release();
    }
};

#endif

