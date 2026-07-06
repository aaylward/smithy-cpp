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

    testImplementation(platform("org.junit:junit-bom:5.11.4"))
    testImplementation("org.junit.jupiter:junit-jupiter")
    testRuntimeOnly("org.junit.platform:junit-platform-launcher")
}
