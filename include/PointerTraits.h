#ifndef POINTERTRAITS_H
#define POINTERTRAITS_H

template<typename Type>
struct PointerTrait
{
    //!	result enumerator
    enum { isPointer = 0 /*!< if not a pointer the value is 0 */ };
    typedef Type OriginalType;
};

template<typename Type>
struct PointerTrait<Type *>
{
    //!	result enumerator
    enum { isPointer = 1 /*!< if is a pointer the value is 1 */ };
    typedef Type OriginalType;
};

template<typename PointerType>
struct PointerTypeTrait
{
    typedef PointerType Type;
};

template<typename PointerType>
struct PointerTypeTrait<PointerType[]>
{
    typedef PointerType Type;
};

template<typename T>
struct DefaultDeleter
{
    static void Deletes(T *_raw_pointer)
    {
        if (NULL != _raw_pointer)
            delete _raw_pointer;
    }
};

template<typename T>
struct DefaultDeleter<T[]>
{
    static void Deletes(T *_raw_pointer)
    {
        if (NULL != _raw_pointer)
            delete[] _raw_pointer;
    }
};

template<typename T>
struct CFreeDeleter
{
    static void Deletes(T *_raw_pointer)
    {
        if (NULL != _raw_pointer)
            ::free(_raw_pointer);
    }
};

#endif