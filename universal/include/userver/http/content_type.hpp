#pragma once

/// @file userver/http/content_type.hpp
/// @brief @copybrief http::ContentType

#include <iosfwd>
#include <stdexcept>
#include <string>
#include <string_view>

#include <userver/logging/fwd.hpp>
#include <userver/utils/str_icase.hpp>

USERVER_NAMESPACE_BEGIN

/// HTTP helpers
namespace http {

/// @brief Content-Type parsing error
class MalformedContentType : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// @ingroup userver_universal userver_containers
///
/// @brief Content-Type representation
class ContentType {
public:
    /// Constructor from a single content-type/media-range header value
    /// as per RFC7231.
    /// @{
    /* implicit */ ContentType(std::string_view);
    /* implicit */ ContentType(const std::string&);
    /* implicit */ ContentType(const char*);
    /// @}

    /// Media type (application/json).
    std::string MediaType() const;

    /// "type" token of media type (application).
    const std::string& TypeToken() const;

    /// "subtype" token of media type (json).
    const std::string& SubtypeToken() const;

    /// Whether the "charset" option was explicitly specified.
    bool HasExplicitCharset() const;

    /// Charset (utf-8).
    const std::string& Charset() const;

    /// Value of "q" parameter in range 0--1000.
    int Quality() const;

    /// Whether this media range accepts specified content type.
    /// Differs from equality comparison in wildcard support.
    bool DoesAccept(const ContentType&) const;

    /// Value of "boundary" parameter.
    const std::string& Boundary() const;

    /// Builds a string representation of content-type/media-range
    std::string ToString() const;

private:
    friend logging::LogHelper& operator<<(logging::LogHelper&, const ContentType&);
    friend std::ostream& operator<<(std::ostream&, const ContentType&);

    void BuildStringRepresentation();

    std::string type_;
    std::string subtype_;
    std::string charset_;
    std::string boundary_;
    int quality_;

    std::string string_representation_;
};

bool operator==(const ContentType&, const ContentType&);
bool operator!=(const ContentType&, const ContentType&);

/// Weak ordering for Accept media-ranges checking.
/// Positions less specific types before more specific, so that the most
/// specific type can be matched first.
bool operator<(const ContentType&, const ContentType&);

class ContentTypeHash {
public:
    size_t operator()(const ContentType&) const;

private:
    utils::StrIcaseHash str_hasher_;
};

namespace content_type {

extern const ContentType kApplicationOctetStream;
extern const ContentType kApplicationJson;
extern const ContentType kTextPlain;

}  // namespace content_type
}  // namespace http

USERVER_NAMESPACE_END
