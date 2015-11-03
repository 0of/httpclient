#ifndef STRINGCONVERTOR_H
#define STRINGCONVERTOR_H

#include <string>
#include <vector>

#include "HttpClientModule.h"

// TODO
typedef std::vector<uint8_t> BytesArray;

class HTTPCLIENT_EXPORT ConstBytesArray
{
private:
    const uint8_t *m_data;
    uint32_t m_len;

public:
    ConstBytesArray(const uint8_t *d, uint32_t len)
        : m_data(d)
        , m_len(len)
    {}

public:
    bool IsNull() const { return m_data == NULL || m_len == 0; }
    const uint8_t *Data() const { return m_data; }
    uint32_t Length() const { return m_len; }

public:
    inline uint8_t operator [] (uint32_t i) const{ return m_data[i]; }
};

class HTTPCLIENT_EXPORT StringConvertor
{
public:
    //! format includes
    //! 2x/X
    //! default 2x
    static void FromBytes(const ConstBytesArray& array, std::string& out, const std::string& format = "2x");
    static void FromBytes(const ConstBytesArray& array, std::wstring& out, const std::string& format = "2x");

    //! to UTF 8 encoding
    static void FromString(const std::wstring& input, std::string& out);
};

#endif