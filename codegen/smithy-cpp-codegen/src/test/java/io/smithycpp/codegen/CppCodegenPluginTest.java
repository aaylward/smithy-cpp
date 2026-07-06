package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.ServiceLoader;
import java.util.stream.StreamSupport;
import org.junit.jupiter.api.Test;
import software.amazon.smithy.build.SmithyBuildPlugin;

class CppCodegenPluginTest {

  @Test
  void reservesThePluginName() {
    assertEquals("cpp-codegen", new CppCodegenPlugin().getName());
  }

  @Test
  void isDiscoverableViaServiceLoader() {
    boolean found =
        StreamSupport.stream(ServiceLoader.load(SmithyBuildPlugin.class).spliterator(), false)
            .anyMatch(plugin -> plugin instanceof CppCodegenPlugin);
    assertTrue(found, "CppCodegenPlugin must be registered in META-INF/services");
  }
}
