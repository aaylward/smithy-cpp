description = "Core Smithy to C++ code generator (smithy-build plugin)"

dependencies {
    api("software.amazon.smithy:smithy-codegen-core:1.53.0")
    api("software.amazon.smithy:smithy-build:1.53.0")

    testImplementation(platform("org.junit:junit-bom:5.11.4"))
    testImplementation("org.junit.jupiter:junit-jupiter")
    testRuntimeOnly("org.junit.platform:junit-platform-launcher")
}
