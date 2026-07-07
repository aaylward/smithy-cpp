$version: "2.0"

namespace example.bookstore

use alloy#simpleRestJson

/// A tiny bookstore over the vendor-neutral alloy#simpleRestJson protocol
/// (the same one smithy4s uses). Exercises labels, query, headers, a JSON
/// body, @httpResponseCode, and a modeled error — enough to pin the neutral
/// wire format (X-Error-Type discriminator, no __type body field).
@simpleRestJson
service Bookstore {
    version: "2026-07-07"
    operations: [GetBook, AddBook]
}

@readonly
@http(method: "GET", uri: "/books/{isbn}")
operation GetBook {
    input := {
        @required
        @httpLabel
        isbn: String

        @httpQuery("currency")
        currency: String
    }

    output := {
        @required
        isbn: String

        @required
        title: String

        price: Float
    }

    errors: [BookNotFound]
}

@http(method: "POST", uri: "/books", code: 201)
operation AddBook {
    input := {
        @required
        isbn: String

        @required
        title: String
    }

    output := {
        // Populated from the HTTP status line, not the body (issue #26).
        @required
        @httpResponseCode
        status: Integer

        @required
        isbn: String
    }
}

@error("client")
@httpError(404)
structure BookNotFound {
    @required
    message: String

    @required
    isbn: String
}
