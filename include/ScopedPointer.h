#ifndef SCOPEDPTR_H
#define SCOPEDPTR_H

#include "HttpClientModule.h"
#include "PointerTraits.h"

template<typename _Pointer_Type, typename _Deleter = DefaultDeleter<_Pointer_Type> >
class HTTPCLIENT_EXPORT ScopedPointer
{
    typedef typename PointerTypeTrait<_Pointer_Type>::Type _Given_Pointer_Type;
    typedef typename ScopedPointer<_Pointer_Type, _Deleter> _Self_Type;

public:
    ScopedPointer(_Given_Pointer_Type *_raw_ptr)
        :m_rawPtr(_raw_ptr){}

    ~ScopedPointer()
    {
        _Deleter::Deletes(m_rawPtr);
    }

private:
    ScopedPointer(const _Self_Type&);
    _Self_Type & operator = (const _Self_Type&);

public:
    inline _Given_Pointer_Type *operator -> (){ return m_rawPtr; }
    inline const _Given_Pointer_Type *operator -> () const{ return m_rawPtr; }

    inline _Given_Pointer_Type &operator * (){ return *m_rawPtr; }
    inline const _Given_Pointer_Type &operator * () const{ return *m_rawPtr; }

    inline operator bool() { return m_rawPtr != NULL; }

public:
    _Given_Pointer_Type *GetRaw() const { return m_rawPtr; }
    void Dismiss() { m_rawPtr = NULL; }

private:
    _Given_Pointer_Type *m_rawPtr;
};

#endif  /*  SCOPEDPTR_H */