package io.smithycpp.codegen;

import java.util.List;
import software.amazon.smithy.build.FileManifest;
import software.amazon.smithy.codegen.core.CodegenContext;
import software.amazon.smithy.codegen.core.SymbolProvider;
import software.amazon.smithy.codegen.core.WriterDelegator;
import software.amazon.smithy.model.Model;

/** Everything the generators need while emitting code. */
public record CppContext(
    Model model,
    CppSettings settings,
    SymbolProvider symbolProvider,
    FileManifest fileManifest,
    WriterDelegator<CppWriter> writerDelegator,
    List<CppIntegration> integrations,
    CppSymbolProvider cppSymbols)
    implements CodegenContext<CppSettings, CppWriter, CppIntegration> {}
