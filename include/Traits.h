#ifndef TRAITS_H
#define TRAITS_H

#include <cstdint>

//!	traits
#define DefHasMethod(methodName) \
    template<class T> \
class Has##methodName##Method \
    { \
    typedef uint16_t Yes; \
    typedef uint8_t No; \
    \
    static Yes _Tester(char[sizeof(&T::methodName)]); \
    static No _Tester(...); \
    \
    public: \
enum { value = sizeof(Yes) == sizeof(_Tester(0)) }; \
    };

struct ReturnTrait
{
    template<typename Return, typename T, typename Arg>
    static Return Type(Return(T::*)(Arg));

    template<typename Return, typename T>
    static Return Type(Return(T::*)(void));
};

struct Arg0Trait
{
    template<typename Return, typename T, typename Arg>
    static Arg Type(Return(T::*)(Arg));

    template<typename Return, typename T>
    static void Type(Return(T::*)(void));
};

template<class Base, class Derived>
struct IsBaseOfTester
{
private:
    typedef uint16_t Yes;
    typedef uint8_t No;

private:
    static Yes Selects(Base *base);
    static No Selects(...);
    static Derived *Marker();

public:
    enum{ Value = (sizeof(Selects(Marker())) == sizeof(Yes)) };
};

template<class Type, Type Value>
struct IntegralConstant
{
    static const Type value = Value;

    typedef Type ValueType;
    typedef IntegralConstant<Type, Value> type;

    operator ValueType() const
    {
        return (value);
    }
};

template<class Base, class Derived>
struct IsBaseOf : public IntegralConstant<bool, IsBaseOfTester<Base, Derived>::Value>
{};

typedef IntegralConstant<bool, true> TrueType;
typedef IntegralConstant<bool, false> FalseType;

//
//	Only classes
//	NOT include primitive types
//
template<typename T1, typename T2>
class CanConvert
{
    typedef uint16_t Yes;
    typedef uint8_t No;

    static Yes _Tester(T2 *);
    static No _Tester(...);

public:
    enum { value = sizeof(Yes) == sizeof(_Tester(static_cast<T1 *>(0))) };	//!	method name
};

template<typename T>
struct IsVoid { enum { value = 1 }; };
template<>
struct IsVoid<void> { enum { value = 0 }; };

template<template<typename TArg> class TemplateClass, class T>
class ReturnIsTemplate	//!	can convert
{
    typedef uint16_t Yes;
    typedef uint8_t No;

    template<typename T>
    static Yes _Tester(TemplateClass<T>);
    static No _Tester(...);

public:
    enum { value = sizeof(Yes) == sizeof(_Tester(ReturnTrait::Type(&T::OnResult))) };	//!	method name
};

#endif