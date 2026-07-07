package io.smithycpp.codegen;

import java.util.HashSet;
import java.util.Set;
import software.amazon.smithy.build.PluginContext;
import software.amazon.smithy.build.SmithyBuildPlugin;
import software.amazon.smithy.codegen.core.CodegenException;
import software.amazon.smithy.codegen.core.directed.CodegenDirector;
import software.amazon.smithy.model.Model;
import software.amazon.smithy.model.neighbor.Walker;
import software.amazon.smithy.model.shapes.Shape;
import software.amazon.smithy.model.shapes.ShapeId;

/**
 * Entry point for C++ code generation, registered with smithy-build as the {@code cpp-codegen}
 * plugin. Phase 2 scope: data type generation (see docs/PLAN.md).
 */
public final class CppCodegenPlugin implements SmithyBuildPlugin {

  @Override
  public String getName() {
    return "cpp-codegen";
  }

  @Override
  public void execute(PluginContext context) {
    CppSettings settings = CppSettings.fromNode(context.getSettings());
    rejectRecursiveShapes(context.getModel(), settings.service());

    CodegenDirector<CppWriter, CppIntegration, CppContext, CppSettings> runner =
        new CodegenDirector<>();
    runner.directedCodegen(new DirectedCppCodegen());
    runner.integrationClass(CppIntegration.class);
    runner.fileManifest(context.getFileManifest());
    runner.model(context.getModel());
    runner.settings(settings);
    runner.service(settings.service());
    runner.performDefaultCodegenTransforms();
    // Legacy string shapes carrying @enum become real enum shapes, so every
    // enum downstream (types, serde, validation) has one representation.
    runner.changeStringEnumsToEnumShapes(true);
    runner.createDedicatedInputsAndOutputs();
    runner.run();
  }

  /**
   * Recursive shapes need pointer indirection (planned; PLAN Phase 2 note); until that exists, fail
   * generation with a clear message instead of emitting non-compiling code.
   */
  private static void rejectRecursiveShapes(Model model, ShapeId service) {
    Walker walker = new Walker(model);
    for (Shape start : walker.walkShapes(model.expectShape(service))) {
      if (!start.isStructureShape() && !start.isUnionShape()) {
        continue;
      }
      findCycle(model, start.getId(), start.getId(), new HashSet<>());
    }
  }

  private static void findCycle(
      Model model, ShapeId origin, ShapeId current, Set<ShapeId> visited) {
    if (!visited.add(current)) {
      return;
    }
    Shape shape = model.expectShape(current);
    for (var member : shape.members()) {
      ShapeId target = member.getTarget();
      if (target.equals(origin)) {
        throw new CodegenException(
            "cpp-codegen: recursive shapes are not supported yet: "
                + origin
                + " refers back to itself via "
                + current
                + "$"
                + member.getMemberName()
                + ". Break the cycle or wait for boxed-recursion support.");
      }
      Shape targetShape = model.expectShape(target);
      if (targetShape.isStructureShape()
          || targetShape.isUnionShape()
          || targetShape.isListShape()
          || targetShape.isMapShape()) {
        findCycle(model, origin, target, visited);
      }
    }
  }
}
