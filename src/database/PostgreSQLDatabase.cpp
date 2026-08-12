#include "DatabaseBackend.h"
#include "DBBackendFactory.h"
#include "DatabaseServer.h"

#include "core/global.h"
#include "util/DatagramIterator.h"
#include "util/Datagram.h"

#include "dclass/dc/DistributedType.h"
#include "dclass/dc/ArrayType.h"
#include "dclass/dc/Struct.h"
#include "dclass/dc/Method.h"

#include <pqxx/pqxx>

#include <cassert>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std;

static ConfigGroup postgresql_backend_config("postgresql", db_backend_config);
static ConfigVariable<string> db_server("server", "postgresql://127.0.0.1/astron",
                                        postgresql_backend_config);
static ConfigVariable<int> num_workers("workers", 8, postgresql_backend_config);

class ConversionException : public std::exception
{
    std::string m_what;
  public:
    ConversionException(const std::string &what) : m_what(what) { }
    virtual const char *what() const noexcept { return m_what.c_str(); }
};

// ------------------------------------------------------------------------------------------------
// JSON encoding / decoding of dclass values
//
// Field values are stored inside the "fields" jsonb document as native JSON
// values (numbers for numeric types, strings for text types, arrays for
// arrays, nested objects for structs/methods) -- mirroring the MongoDB
// backend's BSON representation.  Since jsonb cannot store \u0000, every
// non-ASCII byte (and every control byte) is written as a \u00XX escape and
// a NUL byte is mapped to U+0100 on the way in and back on the way out, so
// arbitrary binary data round-trips exactly.
// ------------------------------------------------------------------------------------------------

// json_escape appends a double-quoted JSON string literal for raw bytes.
static void json_escape(std::string &out, const uint8_t *data, size_t size)
{
    out += '"';
    for(size_t i = 0; i < size; ++i) {
        uint8_t c = data[i];
        if(c >= 0x20 && c <= 0x7E && c != '"' && c != '\\') {
            out += (char)c;
        } else if(c == 0x00) {
            // jsonb cannot represent \u0000; substitute U+0100 instead.
            out += "\\u0100";
        } else {
            char buf[8];
            int n = snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
            out.append(buf, n);
        }
    }
    out += '"';
}

// json_int appends a JSON integer literal.
static void json_int(std::string &out, int64_t v)
{
    char buf[32];
    auto res = std::to_chars(buf, buf + sizeof(buf), v);
    out.append(buf, res.ptr - buf);
}

// json_double appends a JSON number literal.  Non-finite doubles cannot be
// represented in jsonb, so they are stored as the strings "NaN", "Infinity"
// and "-Infinity" (and converted back when read); the shortest
// round-trip representation is used so doubles survive exactly.
static void json_double(std::string &out, double d)
{
    if(std::isnan(d)) {
        out += "\"NaN\"";
        return;
    }
    if(std::isinf(d)) {
        out += (d > 0) ? "\"Infinity\"" : "\"-Infinity\"";
        return;
    }
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), d);
    out.append(buf, res.ptr - buf);
}

// bamboo2json appends the JSON representation of the packed value at dgi's
// current position, advancing dgi as it goes.
static void bamboo2json(std::string &out, const dclass::DistributedType *type,
                        DatagramIterator &dgi)
{
    switch(type->get_type()) {
    case dclass::Type::T_INT8: {
        json_int(out, dgi.read_int8());
    }
    break;
    case dclass::Type::T_INT16: {
        json_int(out, dgi.read_int16());
    }
    break;
    case dclass::Type::T_INT32: {
        json_int(out, dgi.read_int32());
    }
    break;
    case dclass::Type::T_INT64: {
        json_int(out, dgi.read_int64());
    }
    break;
    case dclass::Type::T_UINT8: {
        json_int(out, dgi.read_uint8());
    }
    break;
    case dclass::Type::T_UINT16: {
        json_int(out, dgi.read_uint16());
    }
    break;
    case dclass::Type::T_UINT32: {
        json_int(out, dgi.read_uint32());
    }
    break;
    case dclass::Type::T_UINT64: {
        // uint64 fields are stored as full unsigned integers (this differs
        // from the MongoDB backend, which stores them as wrapped int64s):
        uint64_t v = dgi.read_uint64();
        char buf[32];
        auto res = std::to_chars(buf, buf + sizeof(buf), v);
        out.append(buf, res.ptr - buf);
    }
    break;
    case dclass::Type::T_CHAR: {
        uint8_t c = dgi.read_uint8();
        json_escape(out, &c, 1);
    }
    break;
    case dclass::Type::T_FLOAT32: {
        json_double(out, dgi.read_float32());
    }
    break;
    case dclass::Type::T_FLOAT64: {
        json_double(out, dgi.read_float64());
    }
    break;
    case dclass::Type::T_STRING: {
        vector<uint8_t> str = dgi.read_data(type->get_size());
        json_escape(out, str.data(), str.size());
    }
    break;
    case dclass::Type::T_VARSTRING: {
        string str = dgi.read_string();
        json_escape(out, reinterpret_cast<const uint8_t *>(str.data()), str.size());
    }
    break;
    case dclass::Type::T_BLOB: {
        vector<uint8_t> blob = dgi.read_data(type->get_size());
        json_escape(out, blob.data(), blob.size());
    }
    break;
    case dclass::Type::T_VARBLOB: {
        vector<uint8_t> blob = dgi.read_blob();
        json_escape(out, blob.data(), blob.size());
    }
    break;
    case dclass::Type::T_ARRAY: {
        const dclass::ArrayType *array = type->as_array();
        out += '[';
        for(size_t i = 0; i < array->get_array_size(); ++i) {
            if(i != 0) {
                out += ',';
            }
            bamboo2json(out, array->get_element_type(), dgi);
        }
        out += ']';
    }
    break;
    case dclass::Type::T_VARARRAY: {
        const dclass::ArrayType *array = type->as_array();
        dgsize_t array_length = dgi.read_size();
        dgsize_t starting_size = dgi.tell();
        out += '[';
        bool first = true;
        while(dgi.tell() != starting_size + array_length) {
            if(!first) {
                out += ',';
            }
            first = false;
            bamboo2json(out, array->get_element_type(), dgi);
            if(dgi.tell() > starting_size + array_length) {
                throw ConversionException("Discovered corrupt array-length tag!");
            }
        }
        out += ']';
    }
    break;
    case dclass::Type::T_STRUCT: {
        const dclass::Struct *s = type->as_struct();
        out += '{';
        for(unsigned int i = 0; i < s->get_num_fields(); ++i) {
            const dclass::Field *field = s->get_field(i);
            if(i != 0) {
                out += ',';
            }
            json_escape(out, reinterpret_cast<const uint8_t *>(field->get_name().c_str()),
                        field->get_name().size());
            out += ':';
            bamboo2json(out, field->get_type(), dgi);
        }
        out += '}';
    }
    break;
    case dclass::Type::T_METHOD: {
        const dclass::Method *m = type->as_method();
        out += '{';
        for(unsigned int i = 0; i < m->get_num_parameters(); ++i) {
            const dclass::Parameter *parameter = m->get_parameter(i);
            string name = parameter->get_name();
            if(name.empty()) {
                name = "_" + std::to_string(i);
            }
            if(i != 0) {
                out += ',';
            }
            json_escape(out, reinterpret_cast<const uint8_t *>(name.c_str()), name.size());
            out += ':';
            bamboo2json(out, parameter->get_type(), dgi);
        }
        out += '}';
    }
    break;
    default: {
        assert(false);
    }
    break;
    }
}

// ------------------------------------------------------------------------------------------------
// A minimal JSON value tree sufficient to read back the documents we write
// (and anything else PostgreSQL's jsonb text output can produce).
// ------------------------------------------------------------------------------------------------

enum class JsonKind {
    null_value,
    bool_value,
    int_value,
    uint64_value,
    double_value,
    string_value,
    array_value,
    object_value
};

struct JsonValue {
    JsonKind kind = JsonKind::null_value;
    bool b = false;
    int64_t i = 0;
    uint64_t u = 0;                  // only for kind == uint64_value
    double d = 0;
    std::string s;                     // decoded bytes
    std::vector<JsonValue> arr;
    std::map<std::string, JsonValue> obj;
};

static void json_skip_ws(const string &j, size_t &pos)
{
    while(pos < j.size() && (j[pos] == ' ' || j[pos] == '\t' || j[pos] == '\n' || j[pos] == '\r')) {
        ++pos;
    }
}

// json_utf8_append appends the UTF-8 encoding of a code point to out.
static void json_utf8_append(string &out, uint32_t cp)
{
    if(cp < 0x80) {
        out += (char)cp;
    } else if(cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if(cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

// json_codepoint_to_bytes converts a code point read from a stored JSON
// string back into the raw bytes it stands for.  This is the inverse of the
// escaping performed by json_escape: code points 0x00-0xFF stand for the
// same-valued byte, and U+0100 stands for a NUL byte.
static void json_codepoint_to_bytes(string &out, uint32_t cp)
{
    if(cp <= 0xFF) {
        out += (char)cp;
    } else if(cp == 0x100) {
        out += '\0';
    } else {
        // Hand-edited data may contain arbitrary characters; keep them as
        // valid UTF-8 rather than failing the whole object.
        json_utf8_append(out, cp);
    }
}

static uint32_t json_hex4(const string &j, size_t &pos)
{
    if(pos + 4 > j.size()) {
        throw ConversionException("Truncated \\u escape in stored value");
    }
    uint32_t cp = 0;
    for(int i = 0; i < 4; ++i) {
        char c = j[pos++];
        cp <<= 4;
        if(c >= '0' && c <= '9') {
            cp |= (uint32_t)(c - '0');
        } else if(c >= 'a' && c <= 'f') {
            cp |= (uint32_t)(c - 'a' + 10);
        } else if(c >= 'A' && c <= 'F') {
            cp |= (uint32_t)(c - 'A' + 10);
        } else {
            throw ConversionException("Invalid \\u escape in stored value");
        }
    }
    return cp;
}

// json_parse_string parses a double-quoted JSON string and returns the
// decoded bytes (see json_codepoint_to_bytes).
static string json_parse_string(const string &j, size_t &pos)
{
    if(pos >= j.size() || j[pos] != '"') {
        throw ConversionException("Expected string in stored value");
    }
    ++pos;
    string out;
    while(pos < j.size()) {
        char c = j[pos++];
        if(c == '"') {
            return out;
        }
        if(c == '\\') {
            if(pos >= j.size()) {
                throw ConversionException("Truncated escape in stored value");
            }
            char e = j[pos++];
            switch(e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp = json_hex4(j, pos);
                if(cp >= 0xD800 && cp <= 0xDBFF) {
                    // Expect a low surrogate half.
                    if(pos + 1 < j.size() && j[pos] == '\\' && j[pos + 1] == 'u') {
                        pos += 2;
                        uint32_t lo = json_hex4(j, pos);
                        if(lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            throw ConversionException("Invalid surrogate pair in stored value");
                        }
                    } else {
                        throw ConversionException("Invalid surrogate pair in stored value");
                    }
                } else if(cp >= 0xDC00 && cp <= 0xDFFF) {
                    throw ConversionException("Unexpected low surrogate in stored value");
                }
                json_codepoint_to_bytes(out, cp);
            }
            break;
            default:
                throw ConversionException("Unknown escape in stored value");
            }
        } else {
            // jsonb's text output renders non-ASCII code points as raw
            // UTF-8, so decode multi-byte sequences back into code points.
            unsigned char uc = static_cast<unsigned char>(c);
            if(uc < 0x80) {
                out += static_cast<char>(uc);
            } else {
                uint32_t cp = 0;
                size_t extra = 0;
                if(uc >= 0xC2 && uc <= 0xDF) {
                    cp = uc & 0x1F;
                    extra = 1;
                } else if(uc >= 0xE0 && uc <= 0xEF) {
                    cp = uc & 0x0F;
                    extra = 2;
                } else if(uc >= 0xF0 && uc <= 0xF4) {
                    cp = uc & 0x07;
                    extra = 3;
                } else {
                    throw ConversionException("Invalid UTF-8 in stored value");
                }
                for(size_t k = 0; k < extra; ++k) {
                    if(pos >= j.size()) {
                        throw ConversionException("Truncated UTF-8 in stored value");
                    }
                    unsigned char cc = static_cast<unsigned char>(j[pos++]);
                    if((cc & 0xC0) != 0x80) {
                        throw ConversionException("Invalid UTF-8 in stored value");
                    }
                    cp = (cp << 6) | (cc & 0x3F);
                }
                json_codepoint_to_bytes(out, cp);
            }
        }
    }
    throw ConversionException("Unterminated string in stored value");
}

static JsonValue json_parse_value(const string &j, size_t &pos);

static JsonValue json_parse_array(const string &j, size_t &pos)
{
    ++pos; // consume '['
    JsonValue value;
    value.kind = JsonKind::array_value;
    for(;;) {
        json_skip_ws(j, pos);
        if(pos < j.size() && j[pos] == ']') {
            ++pos;
            return value;
        }
        value.arr.push_back(json_parse_value(j, pos));
        json_skip_ws(j, pos);
        if(pos < j.size() && j[pos] == ',') {
            ++pos;
            continue;
        }
        if(pos < j.size() && j[pos] == ']') {
            ++pos;
            return value;
        }
        throw ConversionException("Malformed array in stored value");
    }
}

static JsonValue json_parse_object(const string &j, size_t &pos)
{
    ++pos; // consume '{'
    JsonValue value;
    value.kind = JsonKind::object_value;
    for(;;) {
        json_skip_ws(j, pos);
        if(pos < j.size() && j[pos] == '}') {
            ++pos;
            return value;
        }
        if(pos >= j.size() || j[pos] != '"') {
            throw ConversionException("Expected key in stored object");
        }
        string key = json_parse_string(j, pos);
        json_skip_ws(j, pos);
        if(pos >= j.size() || j[pos] != ':') {
            throw ConversionException("Expected ':' in stored object");
        }
        ++pos;
        value.obj[key] = json_parse_value(j, pos);
        json_skip_ws(j, pos);
        if(pos < j.size() && j[pos] == ',') {
            ++pos;
            continue;
        }
        if(pos < j.size() && j[pos] == '}') {
            ++pos;
            return value;
        }
        throw ConversionException("Malformed object in stored value");
    }
}

static JsonValue json_parse_number(const string &j, size_t &pos)
{
    size_t start = pos;
    bool is_double = false;
    while(pos < j.size()) {
        char c = j[pos];
        if(c == '.' || c == 'e' || c == 'E') {
            is_double = true;
        }
        if((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
            ++pos;
        } else {
            break;
        }
    }
    string literal = j.substr(start, pos - start);
    JsonValue value;
    if(is_double) {
        value.kind = JsonKind::double_value;
        value.d = strtod(literal.c_str(), nullptr);
        // Only treat true overflow as an error; underflow to subnormals is
        // legal (jsonb numeric preserves them exactly, and we never write
        // out-of-range literals ourselves).
        if(std::isinf(value.d)) {
            throw ConversionException("Floating-point value out of range in stored value");
        }
    } else {
        // Try int64 first; uint64 fields are stored as full unsigned
        // values, which can exceed INT64_MAX, so fall back to uint64 on
        // overflow:
        errno = 0;
        char *end = nullptr;
        long long v = strtoll(literal.c_str(), &end, 10);
        if(end != literal.c_str() && errno != ERANGE) {
            value.kind = JsonKind::int_value;
            value.i = v;
        } else {
            // Reject negative literals too large for int64 (a sign is never
            // produced by our own writer):
            if(literal[0] == '-') {
                throw ConversionException("Integer out of range in stored value");
            }
            errno = 0;
            end = nullptr;
            unsigned long long uv = strtoull(literal.c_str(), &end, 10);
            if(end == literal.c_str() || errno == ERANGE) {
                throw ConversionException("Integer out of range in stored value");
            }
            value.kind = JsonKind::uint64_value;
            value.u = uv;
        }
    }
    return value;
}

static JsonValue json_parse_value(const string &j, size_t &pos)
{
    json_skip_ws(j, pos);
    if(pos >= j.size()) {
        throw ConversionException("Unexpected end of stored value");
    }
    char c = j[pos];
    if(c == '"') {
        JsonValue value;
        value.kind = JsonKind::string_value;
        value.s = json_parse_string(j, pos);
        return value;
    }
    if(c == '[') {
        return json_parse_array(j, pos);
    }
    if(c == '{') {
        return json_parse_object(j, pos);
    }
    if(c == 't' && j.compare(pos, 4, "true") == 0) {
        pos += 4;
        JsonValue value;
        value.kind = JsonKind::bool_value;
        value.b = true;
        return value;
    }
    if(c == 'f' && j.compare(pos, 5, "false") == 0) {
        pos += 5;
        JsonValue value;
        value.kind = JsonKind::bool_value;
        value.b = false;
        return value;
    }
    if(c == 'n' && j.compare(pos, 4, "null") == 0) {
        pos += 4;
        return JsonValue();
    }
    if(c == '-' || (c >= '0' && c <= '9')) {
        return json_parse_number(j, pos);
    }
    throw ConversionException("Unexpected character in stored value");
}

// handle_json_number mirrors the MongoDB backend's handle_bson_number, with
// one deliberate difference: uint64 fields are stored as full unsigned
// integers rather than wrapped int64s.
template<typename T> T handle_json_number(const JsonValue &value)
{
    int64_t i;
    uint64_t u;
    double d;
    bool is_double = false;
    bool is_uint = false;
    if(value.kind == JsonKind::double_value) {
        d = value.d;
        is_double = true;
    } else if(value.kind == JsonKind::uint64_value) {
        u = value.u;
        is_uint = true;
    } else if(value.kind == JsonKind::int_value) {
        i = value.i;
    } else {
        throw ConversionException("Non-numeric value encountered");
    }

    if(numeric_limits<T>::is_integer && is_double) {
        // We have to convert a double to an integer. Error if the double is
        // not floored.
        double intpart;
        if(modf(d, &intpart) != 0.0) {
            throw ConversionException("Non-integer double encountered");
        }

        // (2 ** 53) - 1
        if(d > 9007199254740991 ||
           d < -9007199254740991) {
            throw ConversionException("Excessively large (or small) double encountered");
        }

        i = static_cast<int64_t>(d);
    } else if(!numeric_limits<T>::is_integer && !is_double) {
        // Integer (or unsigned integer) to double. Simple enough:
        d = is_uint ? static_cast<double>(u) : static_cast<double>(i);
    }

    // Special case: uint64_t.  The stored value may be a full unsigned
    // integer, a signed integer, or (on hand-edited data) a double:
    if(is_same<T, uint64_t>::value) {
        if(is_uint) {
            return u;
        }
        if(is_double) {
            if(d < 0) {
                throw ConversionException("Negative value for unsigned field");
            }
            return static_cast<uint64_t>(i);
        }
        if(i < 0) {
            throw ConversionException("Negative value for unsigned field");
        }
        return static_cast<uint64_t>(i);
    }

    // For floating types, we'll just cast and return:
    if(!numeric_limits<T>::is_integer) {
        return static_cast<T>(d);
    }

    // For everything else, cast if in range:
    if(is_uint) {
        uint64_t max = static_cast<uint64_t>(numeric_limits<T>::max());
        if(u > max) {
            throw ConversionException("Integer is out of range");
        }
        return static_cast<T>(u);
    }

    int64_t max = static_cast<int64_t>(numeric_limits<T>::max());
    int64_t min = static_cast<int64_t>(numeric_limits<T>::min());

    if(i > max || i < min) {
        throw ConversionException("Integer is out of range");
    }

    return static_cast<T>(i);
}

// handle_json_float reads a floating point value, accepting the string
// stand-ins used for non-finite doubles.
static double handle_json_float(const JsonValue &value)
{
    if(value.kind == JsonKind::string_value) {
        if(value.s == "NaN") {
            return std::nan("");
        }
        if(value.s == "Infinity") {
            return INFINITY;
        }
        if(value.s == "-Infinity") {
            return -INFINITY;
        }
        throw ConversionException("Expected numeric value");
    }
    return handle_json_number<double>(value);
}

// json2bamboo mirrors the MongoDB backend's bson2bamboo.
static void json2bamboo(const dclass::DistributedType *type,
                        const string &field_name,
                        const JsonValue &value,
                        Datagram &dg)
{
    try {
        switch(type->get_type()) {
        case dclass::Type::T_INT8: {
            dg.add_int8(handle_json_number<int8_t>(value));
        }
        break;
        case dclass::Type::T_INT16: {
            dg.add_int16(handle_json_number<int16_t>(value));
        }
        break;
        case dclass::Type::T_INT32: {
            dg.add_int32(handle_json_number<int32_t>(value));
        }
        break;
        case dclass::Type::T_INT64: {
            dg.add_int64(handle_json_number<int64_t>(value));
        }
        break;
        case dclass::Type::T_UINT8: {
            dg.add_uint8(handle_json_number<uint8_t>(value));
        }
        break;
        case dclass::Type::T_UINT16: {
            dg.add_uint16(handle_json_number<uint16_t>(value));
        }
        break;
        case dclass::Type::T_UINT32: {
            dg.add_uint32(handle_json_number<uint32_t>(value));
        }
        break;
        case dclass::Type::T_UINT64: {
            dg.add_uint64(handle_json_number<uint64_t>(value));
        }
        break;
        case dclass::Type::T_CHAR: {
            if(value.kind != JsonKind::string_value || value.s.size() != 1) {
                throw ConversionException("Expected single-length string for char field");
            }
            dg.add_uint8(static_cast<uint8_t>(value.s[0]));
        }
        break;
        case dclass::Type::T_FLOAT32: {
            dg.add_float32(static_cast<float>(handle_json_float(value)));
        }
        break;
        case dclass::Type::T_FLOAT64: {
            dg.add_float64(handle_json_float(value));
        }
        break;
        case dclass::Type::T_STRING: {
            if(value.kind != JsonKind::string_value) {
                throw ConversionException("Expected string");
            }
            dg.add_data(value.s);
        }
        break;
        case dclass::Type::T_VARSTRING: {
            if(value.kind != JsonKind::string_value) {
                throw ConversionException("Expected string");
            }
            dg.add_string(value.s);
        }
        break;
        case dclass::Type::T_BLOB: {
            if(value.kind != JsonKind::string_value) {
                throw ConversionException("Expected string");
            }
            dg.add_data(value.s);
        }
        break;
        case dclass::Type::T_VARBLOB: {
            if(value.kind != JsonKind::string_value) {
                throw ConversionException("Expected string");
            }
            vector<uint8_t> bytes(value.s.begin(), value.s.end());
            dg.add_blob(bytes);
        }
        break;
        case dclass::Type::T_ARRAY: {
            if(value.kind != JsonKind::array_value) {
                throw ConversionException("Expected array");
            }
            const dclass::ArrayType *array = type->as_array();
            for(size_t i = 0; i < value.arr.size(); ++i) {
                string name = field_name + "[" + std::to_string(i) + "]";
                json2bamboo(array->get_element_type(), name, value.arr[i], dg);
            }
        }
        break;
        case dclass::Type::T_VARARRAY: {
            if(value.kind != JsonKind::array_value) {
                throw ConversionException("Expected array");
            }
            const dclass::ArrayType *array = type->as_array();
            DatagramPtr newdg = Datagram::create();
            for(size_t i = 0; i < value.arr.size(); ++i) {
                string name = field_name + "[" + std::to_string(i) + "]";
                json2bamboo(array->get_element_type(), name, value.arr[i], *newdg);
            }
            dg.add_blob(newdg);
        }
        break;
        case dclass::Type::T_STRUCT: {
            if(value.kind != JsonKind::object_value) {
                throw ConversionException("Expected sub-document");
            }
            const dclass::Struct *s = type->as_struct();
            size_t fields = s->get_num_fields();
            for(size_t i = 0; i < fields; ++i) {
                const dclass::Field *sub_field = s->get_field(i);
                string name = sub_field->get_name();
                auto it = value.obj.find(name);
                if(it == value.obj.end()) {
                    throw ConversionException("Field '" + name + "' is absent from stored document");
                }
                json2bamboo(sub_field->get_type(), name, it->second, dg);
            }
        }
        break;
        case dclass::Type::T_METHOD: {
            if(value.kind != JsonKind::object_value) {
                throw ConversionException("Expected sub-document");
            }
            const dclass::Method *m = type->as_method();
            size_t parameters = m->get_num_parameters();
            for(size_t i = 0; i < parameters; ++i) {
                const dclass::Parameter *parameter = m->get_parameter(i);
                string name = parameter->get_name();
                if(name.empty()) {
                    name = "_" + std::to_string(i);
                }
                auto it = value.obj.find(name);
                if(it == value.obj.end()) {
                    throw ConversionException("Parameter '" + name + "' is absent from stored document");
                }
                json2bamboo(parameter->get_type(), name, it->second, dg);
            }
        }
        break;
        default: {
            assert(false);
        }
        break;
        }
    } catch(const ConversionException &e) {
        throw ConversionException("Failed to format field '" + field_name + "': " + e.what());
    }
}

// ------------------------------------------------------------------------------------------------
// SQL literal helpers
// ------------------------------------------------------------------------------------------------

// sql_literal returns a single-quoted SQL string literal, escaping embedded
// quotes.  Used for field names in expressions such as: fields - 'name'
static string sql_literal(const string &s)
{
    string out = "'";
    for(char c : s) {
        if(c == '\'') {
            out += "''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

// jsonb_path_literal returns a jsonb path-array literal such as '{"name"}',
// escaping any embedded quotes or backslashes in the name.  Used in
// jsonb_set(fields, '{"name"}', ...) and fields -> '{"name"}'.
static string jsonb_path_literal(const string &name)
{
    string out = "'{\"";
    for(char c : name) {
        if(c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    out += "\"}'";
    return out;
}

// ------------------------------------------------------------------------------------------------
// PostgreSQLDatabase
// ------------------------------------------------------------------------------------------------

class PostgreSQLDatabase : public DatabaseBackend
{
  public:
    PostgreSQLDatabase(ConfigNode dbeconfig, doid_t min_id, doid_t max_id) :
        DatabaseBackend(dbeconfig, min_id, max_id), m_shutdown(false)
    {
        stringstream log_name;
        log_name << "Database-PostgreSQL" << "(Range: [" << min_id << ", " << max_id << "])";
        m_log = new LogCategory("postgresql", log_name.str());

        m_conninfo = db_server.get_rval(m_config);

        // Verify the connection and set up the schema, mirroring the MongoDB
        // backend's creation of the globals document at startup:
        try {
            pqxx::connection conn(m_conninfo);
            pqxx::work tx(conn);
            tx.exec("CREATE TABLE IF NOT EXISTS objects ("
                    "  id bigint PRIMARY KEY,"
                    "  dclass text NOT NULL,"
                    "  fields jsonb NOT NULL"
                    ")").no_rows();
            tx.exec("CREATE TABLE IF NOT EXISTS globals ("
                    "  id bigint PRIMARY KEY,"
                    "  n bigint NOT NULL"
                    ")").no_rows();
            tx.exec("CREATE TABLE IF NOT EXISTS free_ids ("
                    "  doid bigint PRIMARY KEY"
                    ")").no_rows();
            tx.exec("INSERT INTO globals (id, n) VALUES (1, $1) "
                    "ON CONFLICT (id) DO NOTHING",
                    pqxx::params{static_cast<long long>(m_min_id)}).no_rows();
            tx.commit();
        } catch(const pqxx::sql_error &e) {
            m_log->fatal() << "Could not initialize PostgreSQL schema: " << e.what() << endl;
            exit(1);
        } catch(const pqxx::broken_connection &e) {
            m_log->fatal() << "Could not connect to PostgreSQL: " << e.what() << endl;
            exit(1);
        }

        for(int i = 0; i < num_workers.get_rval(m_config); ++i) {
            m_threads.push_back(new thread(bind(&PostgreSQLDatabase::run_thread, this)));
        }
    }

    ~PostgreSQLDatabase()
    {
        // Notify the threads to finish and wait for them to do so:
        {
            unique_lock<mutex> guard(m_lock);
            m_shutdown = true;
            m_cv.notify_all();
        }
        for(auto thread : m_threads) {
            thread->join();
            delete thread;
        }
        delete m_log;
    }

    void submit(DBOperation *operation)
    {
        {
            unique_lock<mutex> guard(m_lock);
            m_operation_queue.push(operation);
        }
        m_cv.notify_one();
    }

  private:
    void run_thread()
    {
        unique_lock<mutex> guard(m_lock);

        unique_ptr<pqxx::connection> conn;
        try {
            conn = make_unique<pqxx::connection>(m_conninfo);
        } catch(const std::exception &e) {
            m_log->error() << "Could not connect to database; will retry: " << e.what() << endl;
        }

        while(true) {
            if(m_operation_queue.size() > 0) {
                DBOperation *op = m_operation_queue.front();
                m_operation_queue.pop();
                guard.unlock();
                try {
                    if(!conn || !conn->is_open()) {
                        conn = make_unique<pqxx::connection>(m_conninfo);
                    }
                    pqxx::work tx(*conn);
                    handle_operation(tx, op);
                    // handle_* methods commit before responding, so if we
                    // get here the operation has already been completed.
                } catch(const pqxx::broken_connection &e) {
                    m_log->error() << "Database connection lost: " << e.what() << endl;
                    conn.reset(); // reconnect on the next operation
                    op->on_failure();
                } catch(const std::exception &e) {
                    m_log->error() << "Unexpected database error: " << e.what() << endl;
                    op->on_failure();
                }
                guard.lock();
            } else if(m_shutdown) {
                break;
            } else {
                m_cv.wait(guard);
            }
        }
    }

    void handle_operation(pqxx::work &tx, DBOperation *operation)
    {
        switch(operation->type()) {
        case DBOperation::OperationType::CREATE_OBJECT:
            handle_create(tx, operation);
            break;
        case DBOperation::OperationType::DELETE_OBJECT:
            handle_delete(tx, operation);
            break;
        case DBOperation::OperationType::GET_OBJECT:
        case DBOperation::OperationType::GET_FIELDS:
            handle_get(tx, operation);
            break;
        case DBOperation::OperationType::SET_FIELDS:
        case DBOperation::OperationType::UPDATE_FIELDS:
            handle_modify(tx, operation);
            break;
        default:
            break;
        }
    }

    void handle_create(pqxx::work &tx, DBOperation *operation)
    {
        // Build the fields document from the initial set of fields:
        string fields;
        try {
            fields = "{";
            bool first = true;
            for(const auto &it : operation->set_fields()) {
                if(!first) {
                    fields += ',';
                }
                first = false;
                json_escape(fields, reinterpret_cast<const uint8_t *>(it.first->get_name().c_str()),
                            it.first->get_name().size());
                fields += ':';
                DatagramPtr dg = Datagram::create();
                dg->add_data(it.second);
                DatagramIterator dgi(dg);
                bamboo2json(fields, it.first->get_type(), dgi);
            }
            fields += '}';
        } catch(const ConversionException &e) {
            m_log->error() << "While formatting " << operation->dclass()->get_name()
                           << " for insertion: " << e.what() << endl;
            operation->on_failure();
            return;
        }

        doid_t doid = assign_doid(tx);
        if(doid == INVALID_DO_ID) {
            operation->on_failure();
            return;
        }

        m_log->trace() << "Inserting new " << operation->dclass()->get_name()
                       << "(" << doid << "): " << fields << endl;

        try {
            tx.exec("INSERT INTO objects (id, dclass, fields) VALUES ($1, $2, $3::jsonb)",
                    pqxx::params{static_cast<long long>(doid),
                                 operation->dclass()->get_name(),
                                 fields}).no_rows();
        } catch(const pqxx::sql_error &e) {
            m_log->error() << "Cannot insert new " << operation->dclass()->get_name()
                           << "(" << doid << "): " << e.what() << endl;
            operation->on_failure();
            return;
        }

        tx.commit();
        operation->on_complete(doid);
    }

    void handle_delete(pqxx::work &tx, DBOperation *operation)
    {
        bool success = false;
        try {
            pqxx::result r = tx.exec("DELETE FROM objects WHERE id = $1",
                                      pqxx::params{static_cast<long long>(operation->doid())});
            success = (r.affected_rows() == 1);
        } catch(const pqxx::sql_error &e) {
            m_log->error() << "Unexpected error while deleting " << operation->doid()
                           << ": " << e.what() << endl;
            operation->on_failure();
            return;
        }

        if(success) {
            free_doid(tx, operation->doid());
            tx.commit();
            operation->on_complete();
        } else {
            m_log->error() << "Tried to delete non-existent doid " << operation->doid() << endl;
            operation->on_failure();
        }
    }

    void handle_get(pqxx::work &tx, DBOperation *operation)
    {
        pqxx::result r;
        try {
            r = tx.exec("SELECT dclass, fields FROM objects WHERE id = $1",
                         pqxx::params{static_cast<long long>(operation->doid())});
        } catch(const pqxx::sql_error &e) {
            m_log->error() << "Unexpected error occurred while trying to"
                           " retrieve object with DOID "
                           << operation->doid() << ": " << e.what() << endl;
            operation->on_failure();
            return;
        }

        if(r.empty()) {
            m_log->warning() << "Got queried for non-existent object with DOID "
                             << operation->doid() << endl;
            operation->on_failure();
            return;
        }

        DBObjectSnapshot *snap = format_snapshot(operation->doid(),
                                                 r[0][0].as<string>(), r[0][1].as<string>());
        if(!snap || !operation->verify_class(snap->m_dclass)) {
            operation->on_failure();
        } else {
            tx.commit();
            operation->on_complete(snap);
        }
    }

    void handle_modify(pqxx::work &tx, DBOperation *operation)
    {
        // Build a single UPDATE expression applying all of the sets and
        // unsets.  PostgreSQL evaluates all SET clauses against the original
        // row, so the operations must be nested into one expression.
        vector<string> set_values;
        string fields_expr = "fields";
        try {
            // Unsets first: (fields - 'a' - 'b')
            for(const auto &it : operation->set_fields()) {
                if(it.second.empty()) {
                    fields_expr += " - " + sql_literal(it.first->get_name());
                }
            }
            // Sets next, innermost last: jsonb_set(jsonb_set(fields, ...), ...)
            for(const auto &it : operation->set_fields()) {
                if(it.second.empty()) {
                    continue;
                }
                DatagramPtr dg = Datagram::create();
                dg->add_data(it.second);
                DatagramIterator dgi(dg);
                string json;
                bamboo2json(json, it.first->get_type(), dgi);
                fields_expr = "jsonb_set(" + fields_expr + ", "
                              + jsonb_path_literal(it.first->get_name()) + ", $"
                              + std::to_string(set_values.size() + 2) + "::jsonb)";
                set_values.push_back(std::move(json));
            }
        } catch(const ConversionException &e) {
            m_log->error() << "While formatting fields for modification: " << e.what() << endl;
            operation->on_failure();
            return;
        }

        // Build the criteria, mirroring the MongoDB backend's use of
        // "fields.<name>: {$exists: false}" and "fields.<name>: <value>":
        string criteria;
        vector<string> criteria_values;
        bool first = true;
        try {
            for(const auto &it : operation->criteria_fields()) {
                string cond;
                if(it.second.empty()) {
                    cond = "NOT (fields ? " + sql_literal(it.first->get_name()) + ")";
                } else {
                    DatagramPtr dg = Datagram::create();
                    dg->add_data(it.second);
                    DatagramIterator dgi(dg);
                    string json;
                    bamboo2json(json, it.first->get_type(), dgi);
                    cond = "fields #> " + jsonb_path_literal(it.first->get_name())
                           + " = $" + std::to_string(set_values.size() + criteria_values.size() + 2)
                           + "::jsonb";
                    criteria_values.push_back(std::move(json));
                }
                if(!first) {
                    criteria += " AND ";
                }
                first = false;
                criteria += cond;
            }
        } catch(const ConversionException &e) {
            m_log->error() << "While formatting criteria field for modification: " << e.what() << endl;
            operation->on_failure();
            return;
        }

        // Atomically fetch the pre-update image (for reverts) alongside the
        // update itself, so criteria mismatches can be detected in one
        // round-trip:
        string sql = "WITH prev AS (SELECT dclass, fields FROM objects WHERE id = $1), "
                     "upd AS (UPDATE objects SET fields = " + fields_expr +
                     " WHERE id = $1";
        if(!criteria.empty()) {
            sql += " AND " + criteria;
        }
        sql += " RETURNING fields) "
               "SELECT prev.dclass, prev.fields AS prev_fields, upd.fields AS new_fields "
               "FROM prev LEFT JOIN upd ON true";

        pqxx::params params;
        params.append(static_cast<long long>(operation->doid()));
        for(const string &v : set_values) {
            params.append(v);
        }
        for(const string &v : criteria_values) {
            params.append(v);
        }

        pqxx::result r;
        try {
            r = tx.exec(sql, params);
        } catch(const pqxx::sql_error &e) {
            m_log->error() << "Unexpected error while modifying " << operation->doid()
                           << ": " << e.what() << endl;
            operation->on_failure();
            return;
        }

        if(r.empty()) {
            // The object does not exist:
            m_log->error() << "Attempted to modify unknown DOID: " << operation->doid() << endl;
            operation->on_failure();
            return;
        }

        auto row = r[0];
        string dclass_name = row[0].as<string>();
        string prev_fields = row[1].as<string>();

        if(row[2].is_null()) {
            // The row exists but does not meet the criteria; return a
            // snapshot of the current state, like MongoDB's follow-up query:
            DBObjectSnapshot *snap = format_snapshot(operation->doid(), dclass_name, prev_fields);
            if(snap && operation->verify_class(snap->m_dclass)) {
                operation->on_criteria_mismatch(snap);
            } else {
                operation->on_failure();
            }
            return;
        }

        // The update succeeded. Verify the object's class, and revert if it
        // turns out to be out of place.  Since the update has not been
        // committed yet, simply failing the operation rolls the whole
        // transaction back, which restores the pre-update image atomically
        // (no separate revert statement is needed):
        const dclass::Class *dclass = g_dcf->get_class_by_name(dclass_name);
        if(!dclass) {
            m_log->error() << "Encountered unknown database object: " << dclass_name
                           << "(" << operation->doid() << ")" << endl;
        } else if(operation->verify_class(dclass)) {
            tx.commit();
            operation->on_complete();
            return;
        }

        m_log->trace() << "Reverting changes made to " << operation->doid() << endl;
        operation->on_failure();
    }

    // Get a DBObjectSnapshot from a JSONB fields document; returns nullptr if failure.
    DBObjectSnapshot *format_snapshot(doid_t doid, const string &dclass_name, const string &fields_json)
    {
        m_log->trace() << "Formatting database snapshot of " << doid << ": "
                       << fields_json << endl;

        const dclass::Class *dclass = g_dcf->get_class_by_name(dclass_name);
        if(!dclass) {
            m_log->error() << "Encountered unknown database object: " << dclass_name
                           << "(" << doid << ")" << endl;
            return nullptr;
        }

        JsonValue fields;
        try {
            size_t pos = 0;
            fields = json_parse_value(fields_json, pos);
            json_skip_ws(fields_json, pos);
            if(pos != fields_json.size() || fields.kind != JsonKind::object_value) {
                throw ConversionException("Expected a JSON object of fields");
            }
        } catch(const ConversionException &e) {
            m_log->error() << "Malformed stored fields document for " << dclass_name
                           << "(" << doid << "): " << e.what() << endl;
            return nullptr;
        }

        DBObjectSnapshot *snap = new DBObjectSnapshot();
        snap->m_dclass = dclass;

        try {
            for(const auto &it : fields.obj) {
                const dclass::Field *field = dclass->get_field_by_name(it.first);
                if(!field) {
                    m_log->warning() << "Encountered unexpected field " << it.first
                                     << " while formatting " << dclass_name
                                     << "(" << doid << "); ignored." << endl;
                    continue;
                }

                DatagramPtr dg = Datagram::create();
                json2bamboo(field->get_type(), field->get_name(), it.second, *dg);
                snap->m_fields[field].resize(dg->size());
                memcpy(snap->m_fields[field].data(), dg->get_data(), dg->size());
            }
        } catch(const ConversionException &e) {
            m_log->error() << "Failed to format " << dclass_name << "(" << doid
                           << "): " << e.what() << endl;
            delete snap;
            return nullptr;
        }

        return snap;
    }

    // We need to pop doids off of the free list if our monotonic counter is exhausted
    doid_t assign_doid(pqxx::work &tx)
    {
        doid_t doid = assign_doid_monotonic(tx);
        if(doid != INVALID_DO_ID) {
            return doid;
        }
        m_log->warning() << "Monotonic doid counter exhausted; using free list." << endl;
        return assign_doid_reuse(tx);
    }

    doid_t assign_doid_monotonic(pqxx::work &tx)
    {
        // Atomically increment the counter if it is still inside the
        // [min_id, max_id] range.  RETURNING yields the new value, so the
        // assigned doid is the pre-increment value (matching the MongoDB
        // backend, which reads the counter before it is incremented):
        try {
            pqxx::result r = tx.exec("UPDATE globals SET n = n + 1 "
                                      "WHERE id = 1 AND n BETWEEN $1 AND $2 RETURNING n",
                                      pqxx::params{static_cast<long long>(m_min_id),
                                                   static_cast<long long>(m_max_id)});
            if(r.empty()) {
                return INVALID_DO_ID;
            }
            int64_t n = r[0][0].as<int64_t>();
            m_log->trace() << "assign_doid_monotonic: got globals element: " << n << endl;
            return static_cast<doid_t>(n - 1);
        } catch(const pqxx::sql_error &e) {
            m_log->error() << "Unexpected error while getting monotonic doid: " << e.what() << endl;
            return INVALID_DO_ID;
        }
    }

    doid_t assign_doid_reuse(pqxx::work &tx)
    {
        // Pop the smallest doid off of the free list.  FOR UPDATE SKIP LOCKED
        // ensures that concurrent pops pick different rows instead of racing
        // over the same one:
        try {
            pqxx::result r = tx.exec("WITH c AS ("
                                      "  SELECT doid FROM free_ids ORDER BY doid "
                                      "  LIMIT 1 FOR UPDATE SKIP LOCKED"
                                      ") DELETE FROM free_ids USING c "
                                      "WHERE free_ids.doid = c.doid RETURNING free_ids.doid");
            if(r.empty()) {
                return INVALID_DO_ID;
            }
            doid_t doid = static_cast<doid_t>(r[0][0].as<int64_t>());
            m_log->trace() << "assign_doid_reuse: got doid: " << doid << endl;
            return doid;
        } catch(const pqxx::sql_error &e) {
            m_log->error() << "Unexpected error while getting reused doid: " << e.what() << endl;
            return INVALID_DO_ID;
        }
    }

    void free_doid(pqxx::work &tx, doid_t doid)
    {
        m_log->trace() << "Returning doid " << doid << " to the free pool..." << endl;
        // ON CONFLICT DO NOTHING guards against a doid being freed twice (the
        // MongoDB backend would push a duplicate, which would collide later
        // at insert time).  A hard error is left to propagate, which aborts
        // the transaction and fails the delete along with it:
        pqxx::result r = tx.exec("INSERT INTO free_ids (doid) VALUES ($1) "
                                 "ON CONFLICT (doid) DO NOTHING",
                                 pqxx::params{static_cast<long long>(doid)});
        if(r.affected_rows() == 0) {
            m_log->trace() << "Doid " << doid << " was already in the free pool." << endl;
        }
    }

  private:
    LogCategory *m_log;

    string m_conninfo;

    bool m_shutdown;
    mutex m_lock;
    condition_variable m_cv;
    queue<DBOperation *> m_operation_queue;
    list<thread *> m_threads;
};

static DBBackendFactoryItem<PostgreSQLDatabase> postgresql_factory("postgresql");
