description = "Core Smithy to C++ code generator (smithy-build plugin)"

val repoRoot: File = rootProject.projectDir.parentFile

fun registerFixtureTask(
    taskName: String,
    modelPath: String,
    service: String,
    cppNamespace: String,
    outputPath: String,
) = tasks.register<JavaExec>(taskName) {
    group = "smithy-cpp"
    description = "Regenerates $outputPath from $modelPath"
    classpath = sourceSets["main"].runtimeClasspath
    mainClass = "io.smithycpp.codegen.CppCodegenRunner"
    args = listOf(
        "--model", File(repoRoot, modelPath).absolutePath,
        "--service", service,
        "--namespace", cppNamespace,
        "--runtime-target", "//runtime:core",
        "--output", File(repoRoot, outputPath).absolutePath,
        "--tests-package", "//" + outputPath,
    )
    doFirst {
        project.delete(File(repoRoot, outputPath))
    }
}

val generateWeatherFixture = registerFixtureTask(
    "generateWeatherFixture",
    "examples/weather/model/weather.smithy",
    "example.weather#Weather",
    "example::weather",
    "examples/weather/generated",
)

val generateCafeFixture = registerFixtureTask(
    "generateCafeFixture",
    "examples/cafe/model/cafe.smithy",
    "example.cafe#Cafe",
    "example::cafe",
    "examples/cafe/generated",
)

// The official protocol-test suite models, kept off the main runtime classpath
// so ordinary fixture generation doesn't assemble them.
val protocolTestModels: Configuration by configurations.creating

fun registerProtocolTestTask(
    taskName: String,
    service: String,
    cppNamespace: String,
    outputPath: String,
    omitOperations: List<String>,
    malformedTests: Boolean = false,
) = tasks.register<JavaExec>(taskName) {
    group = "smithy-cpp"
    description = "Regenerates $outputPath from the official protocol test suite for $service"
    classpath = sourceSets["main"].runtimeClasspath + protocolTestModels
    mainClass = "io.smithycpp.codegen.CppCodegenRunner"
    args = listOf(
        "--service", service,
        "--namespace", cppNamespace,
        "--runtime-target", "//runtime:core",
        "--output", File(repoRoot, outputPath).absolutePath,
        "--tests-package", "//" + outputPath,
    ) + omitOperations.flatMap { listOf("--omit-operation", it) } +
        (if (malformedTests) listOf("--malformed-tests", "true") else emptyList())
    doFirst {
        project.delete(File(repoRoot, outputPath))
    }
}

// Operations pruned from the suites because they use bindings the generator
// does not implement yet (PLAN Phase 3b/4 follow-ups). This list, like the
// test exclusion list, must only shrink.
val generateRestJson1ProtocolTests = registerProtocolTestTask(
    "generateRestJson1ProtocolTests",
    "aws.protocoltests.restjson#RestJson",
    "smithy::protocoltests::restjson",
    "protocol-tests/restjson1/generated",
    malformedTests = true,
    omitOperations = listOf(
        // Recursive shapes need boxed-recursion support (PLAN Phase 2 note).
        "aws.protocoltests.restjson#RecursiveShapes",
        // @streaming payloads are Phase 8 scope.
        "aws.protocoltests.restjson#StreamingTraits",
        "aws.protocoltests.restjson#StreamingTraitsRequireLength",
        "aws.protocoltests.restjson#StreamingTraitsWithMediaType",
    ),
)

val generateRpcv2CborProtocolTests = registerProtocolTestTask(
    "generateRpcv2CborProtocolTests",
    "smithy.protocoltests.rpcv2Cbor#RpcV2Protocol",
    "smithy::protocoltests::rpcv2cbor",
    "protocol-tests/rpcv2cbor/generated",
    malformedTests = true,
    omitOperations = listOf(
        // Recursive shapes need boxed-recursion support (PLAN Phase 2 note).
        "smithy.protocoltests.rpcv2Cbor#RecursiveShapes",
    ),
)

// The constraint-validation suite: httpMalformedRequestTests only (no
// httpRequestTests/httpResponseTests), so malformed generation is enabled for
// this module. The main suites' malformed tests (parser strictness) are a
// Phase 4d follow-up.
val generateRestJson1ValidationProtocolTests = registerProtocolTestTask(
    "generateRestJson1ValidationProtocolTests",
    "aws.protocoltests.restjson.validation#RestJsonValidation",
    "smithy::protocoltests::restjsonvalidation",
    "protocol-tests/restjson1-validation/generated",
    listOf(
        // Recursive shapes need boxed-recursion support (PLAN Phase 2 note).
        "aws.protocoltests.restjson.validation#RecursiveStructures",
    ),
    malformedTests = true,
)

tasks.register("generateProtocolTests") {
    group = "smithy-cpp"
    description = "Regenerates the checked-in protocol conformance suites under protocol-tests/"
    dependsOn(
        generateRestJson1ProtocolTests,
        generateRpcv2CborProtocolTests,
        generateRestJson1ValidationProtocolTests,
    )
}

tasks.register("generateFixtures") {
    group = "smithy-cpp"
    description = "Regenerates all checked-in generated code under examples/ (the goldens)"
    dependsOn(generateWeatherFixture, generateCafeFixture)
}

tasks.withType<Test>().configureEach {
    systemProperty("smithycpp.repoRoot", repoRoot.absolutePath)
}

dependencies {
    api("software.amazon.smithy:smithy-codegen-core:1.53.0")
    api("software.amazon.smithy:smithy-build:1.53.0")
    // Trait definitions used by the fixture models.
    implementation("software.amazon.smithy:smithy-aws-traits:1.53.0")
    implementation("software.amazon.smithy:smithy-protocol-traits:1.53.0")
    // smithy.test#http{Request,Response}Tests trait definitions.
    implementation("software.amazon.smithy:smithy-protocol-test-traits:1.53.0")
    // The official conformance suite models (generateProtocolTests classpath only).
    protocolTestModels("software.amazon.smithy:smithy-aws-protocol-tests:1.53.0")
    protocolTestModels("software.amazon.smithy:smithy-protocol-tests:1.53.0")

    testImplementation(platform("org.junit:junit-bom:5.11.4"))
    testImplementation("org.junit.jupiter:junit-jupiter")
    testRuntimeOnly("org.junit.platform:junit-platform-launcher")
}

// Local iteration helper (not for CI): prints the protocol-test runner classpath.
tasks.register("printProtocolTestClasspath") {
    doLast {
        println((sourceSets["main"].runtimeClasspath + protocolTestModels).asPath)
    }
}
