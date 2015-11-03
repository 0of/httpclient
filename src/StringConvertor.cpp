#include "StringConvertor.h"

void StringConvertor::FromBytes(const ConstBytesArray& array, std::string& out, const std::string& format)
{
    static const char *HexTableU = "0123456789ABCDEF";
    static const char *HexTableL = "0123456789abcdef";

    const char *HexTable = format == "2x" ? HexTableL : HexTableU;

    std::string &string = out;
    if (array.IsNull())
        return;

    string.resize(array.Length() * 2);

    for (uint32_t i = 0; i != array.Length(); ++i)
    {
        uint8_t byte = array[i];

        string[i * 2] = HexTable[(byte >> 4) & 0x0F];
        string[i * 2 + 1] = HexTable[byte & 0x0F];
    }
}

void StringConvertor::FromBytes(const ConstBytesArray& array, std::wstring& out, const std::string& format)
{
    static const wchar_t *HexTableU = L"0123456789ABCDEF";
    static const wchar_t *HexTableL = L"0123456789abcdef";

    const wchar_t *HexTable = format == "2x" ? HexTableL : HexTableU;

    std::wstring &string = out;
    if (array.IsNull())
        return;

    string.resize(array.Length() * 2);

    for (uint32_t i = 0; i != array.Length(); ++i)
    {
        uint8_t byte = array[i];

        string[i * 2] = HexTable[(byte >> 4) & 0x0F];
        string[i * 2 + 1] = HexTable[byte & 0x0F];
    }
}

void StringConvertor::FromString(const std::wstring& input, std::string& out)
{
    out.reserve(input.size() * (sizeof(wchar_t) / sizeof(char)));
    const wchar_t *src = input.c_str();

    for (auto i = 0; input.size() != i; ++i)
    {
        uint32_t each = *(src + i);

        if (each <= 0x7F)
        {
            out.push_back(static_cast<char>(each));
        }
        else if (each <= 0x7FF)
        {
            out.push_back(0xC0 | ((each >> 6) & 0x1F));
            out.push_back(0x80 | (each & 0x3F));
        }
        else if (each <= 0xFFFF)
        {
            out.push_back(0xe0 | ((each >> 12) & 0x0F));
            out.push_back(0x80 | ((each >> 6) & 0x3F));
            out.push_back(0x80 | (each & 0x3F));
        }
        else if (each <= 0x10FFFF)
        {
            out.push_back(0xF0 | ((each >> 18) & 0x07));
            out.push_back(0x80 | ((each >> 12) & 0x3F));
            out.push_back(0x80 | ((each >> 6) & 0x3F));
            out.push_back(0x80 | (each & 0x3F));
        }
        else
        {
            //! TO-DO
            throw 0;
        }
    }
}