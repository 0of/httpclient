#ifndef REFSHAREDPOINTER_H
#define REFSHAREDPOINTER_H

#include "HttpClientModule.h"
#include "PointerTraits.h"

template<typename T, typename Deleter = DefaultDeleter<T> >
class HTTPCLIENT_EXPORT RefSharedPointer
{
public:
    typedef typename PointerTypeTrait<T>::Type PointerType;
    typedef typename RefSharedPointer<T, Deleter> SelfType;

private:
    PointerType *m_rawPtr;

public:
    RefSharedPointer()
        : m_rawPtr(NULL)
    {
    }

    RefSharedPointer(T* ptr)
        : m_rawPtr(ptr)
    {
    }

    RefSharedPointer(const SelfType& t)
        : m_rawPtr(t.m_rawPtr)
    {
        if (m_rawPtr)
            m_rawPtr->m_ref.AddRef();
    }

    ~RefSharedPointer()
    {
        if (m_rawPtr)
        {
            if (m_rawPtr->m_ref.Release())
            {
                Deleter::Deletes(m_rawPtr);
                m_rawPtr = NULL;
            }
        }
    }

public:
    inline PointerType *operator -> (){ return m_rawPtr; }
    inline const PointerType *operator -> () const{ return m_rawPtr; }

    inline PointerType &operator * (){ return *m_rawPtr; }
    inline const PointerType &operator * () const{ return *m_rawPtr; }

public:
    operator bool() const { return NULL != m_rawPtr; }
    bool operator == (PointerType *ptr) const { return ptr == m_rawPtr }
    bool operator != (PointerType *ptr) const { return ptr != m_rawPtr }

    bool operator == (const SelfType& ptr) const { return ptr.m_rawPtr == m_rawPtr }
    bool operator != (const SelfType& ptr) const { return ptr.m_rawPtr != m_rawPtr }

    SelfType& operator = (const SelfType& t)
    {
        if (m_rawPtr)
        {
            if (m_rawPtr->m_ref.Release())
            {
                Deleter::Deletes(m_rawPtr);
                m_rawPtr = NULL;
            }
        }

        if (t.m_rawPtr)
        {
            m_rawPtr = t.m_rawPtr;
            m_rawPtr->m_ref.AddRef();
        }

        return *this;
    }
};

#endif