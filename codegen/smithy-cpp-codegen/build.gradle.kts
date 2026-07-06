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
        "--protocol-tests-package", "//" + outputPath,
    ) + omitOperations.flatMap { listOf("--omit-operation", it) }
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
    listOf(
        // Recursive shapes need boxed-recursion support (PLAN Phase 2 note).
        "aws.protocoltests.restjson#RecursiveShapes",
        // @httpPayload bindings (input and output) are not implemented yet.
        "aws.protocoltests.restjson#DocumentTypeAsPayload",
        "aws.protocoltests.restjson#HttpEnumPayload",
        "aws.protocoltests.restjson#HttpPayloadTraits",
        "aws.protocoltests.restjson#HttpPayloadTraitsWithMediaType",
        "aws.protocoltests.restjson#HttpPayloadWithStructure",
        "aws.protocoltests.restjson#HttpPayloadWithUnion",
        "aws.protocoltests.restjson#HttpStringPayload",
        "aws.protocoltests.restjson#MalformedAcceptWithGenericString",
        "aws.protocoltests.restjson#MalformedAcceptWithPayload",
        "aws.protocoltests.restjson#MalformedContentTypeWithGenericString",
        "aws.protocoltests.restjson#MalformedContentTypeWithPayload",
        "aws.protocoltests.restjson#TestPayloadBlob",
        "aws.protocoltests.restjson#TestPayloadStructure",
        // @streaming payloads are Phase 8 scope.
        "aws.protocoltests.restjson#StreamingTraits",
        "aws.protocoltests.restjson#StreamingTraitsRequireLength",
        "aws.protocoltests.restjson#StreamingTraitsWithMediaType",
        // @httpPrefixHeaders bindings are not implemented yet.
        "aws.protocoltests.restjson#HttpPrefixHeaders",
        "aws.protocoltests.restjson#HttpPrefixHeadersInResponse",
        // @httpResponseCode bindings are not implemented yet.
        "aws.protocoltests.restjson#HttpResponseCode",
        "aws.protocoltests.restjson#ResponseCodeRequired",
    ),
)

val generateRpcv2CborProtocolTests = registerProtocolTestTask(
    "generateRpcv2CborProtocolTests",
    "smithy.protocoltests.rpcv2Cbor#RpcV2Protocol",
    "smithy::protocoltests::rpcv2cbor",
    "protocol-tests/rpcv2cbor/generated",
    listOf(
        // Recursive shapes need boxed-recursion support (PLAN Phase 2 note).
        "smithy.protocoltests.rpcv2Cbor#RecursiveShapes",
    ),
)

tasks.register("generateProtocolTests") {
    group = "smithy-cpp"
    description = "Regenerates the checked-in protocol conformance suites under protocol-tests/"
    dependsOn(generateRestJson1ProtocolTests, generateRpcv2CborProtocolTests)
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
