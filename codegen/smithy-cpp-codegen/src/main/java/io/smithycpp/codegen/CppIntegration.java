package io.smithycpp.codegen;

import software.amazon.smithy.codegen.core.SmithyIntegration;

/** Extension point for customizing C++ code generation (none built in yet). */
public interface CppIntegration extends SmithyIntegration<CppSettings, CppWriter, CppContext> {}
