$version: "2.0"

namespace acme.todo

use aws.protocols#restJson1

/// A deliberately small service: enough to show routing, bodies, constraint
/// validation, and a modeled error end to end.
@restJson1
service Todo {
    version: "2026-01-01"
    operations: [AddTask, GetTask]
}

@http(method: "POST", uri: "/tasks")
operation AddTask {
    input := {
        @required
        @length(min: 1, max: 256)
        title: String
    }

    output := {
        @required
        taskId: String

        @required
        title: String
    }
}

@readonly
@http(method: "GET", uri: "/tasks/{taskId}")
operation GetTask {
    input := {
        @required
        @httpLabel
        taskId: String
    }

    output := {
        @required
        taskId: String

        @required
        title: String

        done: Boolean
    }

    errors: [NoSuchTask]
}

@error("client")
@httpError(404)
structure NoSuchTask {
    @required
    message: String
}
