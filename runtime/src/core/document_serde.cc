#include "smithy/core/document_serde.h"

#include "smithy/core/base64.h"

namespace smithy {

Outcome<Timestamp> TimestampFromDocument(const Document& doc, TimestampFormat format) {
  if (doc.is_timestamp()) {
    return doc.as_timestamp().value;
  }
  if (doc.is_int() || doc.is_double()) {
    if (format != TimestampFormat::kEpochSeconds) {
      return Error::Serialization("timestamp: numeric value for a string-formatted timestamp");
    }
    return Timestamp::FromEpochSeconds(doc.AsNumber());
  }
  if (doc.is_string()) {
    if (format == TimestampFormat::kEpochSeconds) {
      return Error::Serialization("timestamp: string value for an epoch-seconds timestamp");
    }
    return Timestamp::Parse(doc.as_string(), format);
  }
  return Error::Serialization("timestamp: expected a timestamp, number, or string");
}

Outcome<Blob> BlobFromDocument(const Document& doc) {
  if (doc.is_blob()) {
    return doc.as_blob();
  }
  if (doc.is_string()) {
    return Base64Decode(doc.as_string());
  }
  return Error::Serialization("blob: expected a byte string or base64 text");
}

}  // namespace smithy
