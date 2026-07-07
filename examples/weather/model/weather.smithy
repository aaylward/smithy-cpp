$version: "2.0"

namespace example.weather

use aws.protocols#restJson1

/// Provides weather forecasts for cities.
///
/// REST fixture model: exercises HTTP bindings (labels, query params, headers),
/// resources, pagination, and modeled errors. Used by codegen golden tests and
/// the client<->server integration harness (docs/PLAN.md, Phases 2-5).
@restJson1
@title("Weather Service")
@paginated(inputToken: "nextToken", outputToken: "nextToken", pageSize: "pageSize")
service Weather {
    version: "2026-07-06"
    resources: [City]
    operations: [GetCurrentTime]
}

resource City {
    identifiers: { cityId: CityId }
    read: GetCity
    list: ListCities
    delete: DeleteCity
    resources: [Forecast]
}

resource Forecast {
    identifiers: { cityId: CityId }
    read: GetForecast
}

@pattern("^[A-Za-z0-9 ]+$")
string CityId

@readonly
@http(method: "GET", uri: "/cities/{cityId}")
operation GetCity {
    input := {
        @required
        @httpLabel
        cityId: CityId
    }

    output := {
        @required
        name: String

        @required
        coordinates: CityCoordinates
    }

    errors: [NoSuchResource]
}

/// Deletes a city: a 204 No Content operation (the response has no body).
@idempotent
@http(method: "DELETE", uri: "/cities/{cityId}", code: 204)
operation DeleteCity {
    input := {
        @required
        @httpLabel
        cityId: CityId
    }

    errors: [NoSuchResource]
}

structure CityCoordinates {
    @required
    latitude: Float

    @required
    longitude: Float
}

@readonly
@paginated(items: "items")
@http(method: "GET", uri: "/cities")
operation ListCities {
    input := {
        @httpQuery("nextToken")
        nextToken: String

        @httpQuery("pageSize")
        pageSize: Integer
    }

    output := {
        nextToken: String

        @required
        items: CitySummaries
    }
}

list CitySummaries {
    member: CitySummary
}

structure CitySummary {
    @required
    cityId: CityId

    @required
    name: String
}

@readonly
@http(method: "GET", uri: "/cities/{cityId}/forecast")
operation GetForecast {
    input := {
        @required
        @httpLabel
        cityId: CityId
    }

    output := {
        chanceOfRain: Float
    }

    errors: [NoSuchResource]
}

@readonly
@http(method: "GET", uri: "/current-time")
operation GetCurrentTime {
    output := {
        @required
        time: Timestamp
    }
}

/// The requested resource does not exist.
@error("client")
@httpError(404)
structure NoSuchResource {
    @required
    resourceType: String
}
