package io.smithycpp.codegen;

import software.amazon.smithy.codegen.core.SymbolProvider;
import software.amazon.smithy.codegen.core.WriterDelegator;
import software.amazon.smithy.codegen.core.directed.CreateContextDirective;
import software.amazon.smithy.codegen.core.directed.CreateSymbolProviderDirective;
import software.amazon.smithy.codegen.core.directed.DirectedCodegen;
import software.amazon.smithy.codegen.core.directed.GenerateEnumDirective;
import software.amazon.smithy.codegen.core.directed.GenerateErrorDirective;
import software.amazon.smithy.codegen.core.directed.GenerateIntEnumDirective;
import software.amazon.smithy.codegen.core.directed.GenerateServiceDirective;
import software.amazon.smithy.codegen.core.directed.GenerateStructureDirective;
import software.amazon.smithy.codegen.core.directed.GenerateUnionDirective;

/**
 * Directs C++ code generation. Phase 2 scope: data types only (structures, errors, unions, enums),
 * plus the generated module's BUILD file. Shapes arrive in topological order, so the single types.h
 * needs no forward declarations.
 */
public final class DirectedCppCodegen
    implements DirectedCodegen<CppContext, CppSettings, CppIntegration> {

  @Override
  public SymbolProvider createSymbolProvider(CreateSymbolProviderDirective<CppSettings> directive) {
    return new CppSymbolProvider(directive.model(), directive.settings());
  }

  @Override
  public CppContext createContext(CreateContextDirective<CppSettings, CppIntegration> directive) {
    return new CppContext(
        directive.model(),
        directive.settings(),
        directive.symbolProvider(),
        directive.fileManifest(),
        new WriterDelegator<>(
            directive.fileManifest(),
            directive.symbolProvider(),
            new CppWriter.Factory(directive.settings())),
        directive.integrations(),
        new CppSymbolProvider(directive.model(), directive.settings()));
  }

  @Override
  public void generateService(GenerateServiceDirective<CppContext, CppSettings> directive) {
    software.amazon.smithy.model.shapes.ServiceShape service = directive.shape();
    ProtocolGenerator protocol = resolveProtocol(service);

    SerdeGenerator serdeGenerator = new SerdeGenerator(directive.context());
    boolean hasSerde = !serdeGenerator.serdeShapes().isEmpty();
    if (hasSerde) {
      serdeGenerator.run();
    }

    boolean hasClient = false;
    if (protocol != null) {
      ClientGenerator clientGenerator = new ClientGenerator(directive.context(), service, protocol);
      if (!clientGenerator.operations().isEmpty()) {
        clientGenerator.run();
        hasClient = true;
      }
    }
    BuildFileGenerator.run(directive.context(), protocol, hasClient, hasSerde);
  }

  /** Null when the service declares no supported protocol (types+serde still generate). */
  private static ProtocolGenerator resolveProtocol(
      software.amazon.smithy.model.shapes.ServiceShape service) {
    if (service.hasTrait(software.amazon.smithy.aws.traits.protocols.RestJson1Trait.class)) {
      return new RestJson1Protocol();
    }
    if (service.hasTrait(software.amazon.smithy.protocol.traits.Rpcv2CborTrait.class)) {
      return new Rpcv2CborProtocol();
    }
    return null;
  }

  @Override
  public void generateStructure(GenerateStructureDirective<CppContext, CppSettings> directive) {
    if (directive.shape().getId().toString().equals("smithy.api#Unit")) {
      return; // Maps to the runtime's smithy::Unit; nothing to declare.
    }
    directive
        .context()
        .writerDelegator()
        .useShapeWriter(
            directive.shape(),
            writer ->
                new TypeGenerators(directive.context(), writer)
                    .generateStructure(directive.shape()));
  }

  @Override
  public void generateError(GenerateErrorDirective<CppContext, CppSettings> directive) {
    // Errors are plain structures until protocol-aware error handling lands in Phase 3.
    directive
        .context()
        .writerDelegator()
        .useShapeWriter(
            directive.shape(),
            writer ->
                new TypeGenerators(directive.context(), writer)
                    .generateStructure(directive.shape()));
  }

  @Override
  public void generateUnion(GenerateUnionDirective<CppContext, CppSettings> directive) {
    directive
        .context()
        .writerDelegator()
        .useShapeWriter(
            directive.shape(),
            writer ->
                new TypeGenerators(directive.context(), writer).generateUnion(directive.shape()));
  }

  @Override
  public void generateEnumShape(GenerateEnumDirective<CppContext, CppSettings> directive) {
    directive
        .context()
        .writerDelegator()
        .useShapeWriter(
            directive.shape(),
            writer ->
                new TypeGenerators(directive.context(), writer)
                    .generateEnum(directive.shape().asEnumShape().orElseThrow()));
  }

  @Override
  public void generateIntEnumShape(GenerateIntEnumDirective<CppContext, CppSettings> directive) {
    directive
        .context()
        .writerDelegator()
        .useShapeWriter(
            directive.shape(),
            writer ->
                new TypeGenerators(directive.context(), writer)
                    .generateIntEnum(
                        (software.amazon.smithy.model.shapes.IntEnumShape) directive.shape()));
  }
}
