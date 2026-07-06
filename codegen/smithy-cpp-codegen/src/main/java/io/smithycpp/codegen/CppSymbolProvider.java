package io.smithycpp.codegen;

import java.util.Set;
import java.util.TreeSet;
import software.amazon.smithy.codegen.core.CodegenException;
import software.amazon.smithy.codegen.core.Symbol;
import software.amazon.smithy.codegen.core.SymbolProvider;
import software.amazon.smithy.model.Model;
import software.amazon.smithy.model.shapes.BigDecimalShape;
import software.amazon.smithy.model.shapes.BigIntegerShape;
import software.amazon.smithy.model.shapes.BlobShape;
import software.amazon.smithy.model.shapes.BooleanShape;
import software.amazon.smithy.model.shapes.ByteShape;
import software.amazon.smithy.model.shapes.DocumentShape;
import software.amazon.smithy.model.shapes.DoubleShape;
import software.amazon.smithy.model.shapes.EnumShape;
import software.amazon.smithy.model.shapes.FloatShape;
import software.amazon.smithy.model.shapes.IntEnumShape;
import software.amazon.smithy.model.shapes.IntegerShape;
import software.amazon.smithy.model.shapes.ListShape;
import software.amazon.smithy.model.shapes.LongShape;
import software.amazon.smithy.model.shapes.MapShape;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.OperationShape;
import software.amazon.smithy.model.shapes.ResourceShape;
import software.amazon.smithy.model.shapes.ServiceShape;
import software.amazon.smithy.model.shapes.Shape;
import software.amazon.smithy.model.shapes.ShapeVisitor;
import software.amazon.smithy.model.shapes.ShortShape;
import software.amazon.smithy.model.shapes.StringShape;
import software.amazon.smithy.model.shapes.StructureShape;
import software.amazon.smithy.model.shapes.TimestampShape;
import software.amazon.smithy.model.shapes.UnionShape;
import software.amazon.smithy.model.traits.SparseTrait;

/**
 * Maps Smithy shapes to C++ types (the naming contract in docs/generated-types.md).
 *
 * <p>Each returned {@link Symbol}'s name is the full C++ type text; the headers it needs are
 * carried in the {@code headers} property (angle form {@code <vector>} or quote form {@code
 * "smithy/core/blob.h"}).
 */
final class CppSymbolProvider implements SymbolProvider {

  static final String HEADERS_PROPERTY = "headers";

  private final Model model;
  private final CppSettings settings;
  private final Visitor visitor;

  CppSymbolProvider(Model model, CppSettings settings) {
    this.model = model;
    this.settings = settings;
    this.visitor = new Visitor();
  }

  @Override
  public Symbol toSymbol(Shape shape) {
    return shape.accept(visitor);
  }

  @Override
  public String toMemberName(MemberShape member) {
    return CppReservedWords.escape(member.getMemberName());
  }

  /** Full member type text: the target type, wrapped in std::optional unless @required. */
  Symbol toMemberSymbol(MemberShape member) {
    Symbol target = toSymbol(model.expectShape(member.getTarget()));
    if (member.isRequired()) {
      return target;
    }
    Set<String> headers = new TreeSet<>(headersOf(target));
    headers.add("<optional>");
    return builder("std::optional<" + target.getName() + ">", headers).build();
  }

  @SuppressWarnings("unchecked")
  static Set<String> headersOf(Symbol symbol) {
    return symbol.getProperty(HEADERS_PROPERTY).map(value -> (Set<String>) value).orElse(Set.of());
  }

  private Symbol.Builder builder(String name, Set<String> headers) {
    return Symbol.builder().name(name).putProperty(HEADERS_PROPERTY, headers);
  }

  private Symbol declared(Shape shape) {
    String name = CppReservedWords.escape(shape.getId().getName());
    return builder(name, Set.of("\"" + settings.includePrefix() + "/types.h\""))
        .definitionFile(settings.typesHeaderFile())
        .build();
  }

  private final class Visitor extends ShapeVisitor.Default<Symbol> {

    @Override
    protected Symbol getDefault(Shape shape) {
      throw new CodegenException(
          "cpp-codegen: shape type not supported yet: "
              + shape.getType()
              + " ("
              + shape.getId()
              + ")");
    }

    @Override
    public Symbol blobShape(BlobShape shape) {
      return builder("smithy::Blob", Set.of("\"smithy/core/blob.h\"")).build();
    }

    @Override
    public Symbol booleanShape(BooleanShape shape) {
      return builder("bool", Set.of()).build();
    }

    @Override
    public Symbol byteShape(ByteShape shape) {
      return builder("std::int8_t", Set.of("<cstdint>")).build();
    }

    @Override
    public Symbol shortShape(ShortShape shape) {
      return builder("std::int16_t", Set.of("<cstdint>")).build();
    }

    @Override
    public Symbol integerShape(IntegerShape shape) {
      return builder("std::int32_t", Set.of("<cstdint>")).build();
    }

    @Override
    public Symbol longShape(LongShape shape) {
      return builder("std::int64_t", Set.of("<cstdint>")).build();
    }

    @Override
    public Symbol floatShape(FloatShape shape) {
      return builder("float", Set.of()).build();
    }

    @Override
    public Symbol doubleShape(DoubleShape shape) {
      return builder("double", Set.of()).build();
    }

    @Override
    public Symbol bigIntegerShape(BigIntegerShape shape) {
      throw new CodegenException("cpp-codegen: bigInteger is not supported yet: " + shape.getId());
    }

    @Override
    public Symbol bigDecimalShape(BigDecimalShape shape) {
      throw new CodegenException("cpp-codegen: bigDecimal is not supported yet: " + shape.getId());
    }

    @Override
    public Symbol stringShape(StringShape shape) {
      return builder("std::string", Set.of("<string>")).build();
    }

    @Override
    public Symbol enumShape(EnumShape shape) {
      return declared(shape);
    }

    @Override
    public Symbol intEnumShape(IntEnumShape shape) {
      return declared(shape);
    }

    @Override
    public Symbol timestampShape(TimestampShape shape) {
      return builder("smithy::Timestamp", Set.of("\"smithy/core/timestamp.h\"")).build();
    }

    @Override
    public Symbol documentShape(DocumentShape shape) {
      return builder("smithy::Document", Set.of("\"smithy/core/document.h\"")).build();
    }

    @Override
    public Symbol listShape(ListShape shape) {
      Symbol inner = elementSymbol(shape.getMember(), shape.hasTrait(SparseTrait.class));
      Set<String> headers = new TreeSet<>(headersOf(inner));
      headers.add("<vector>");
      return builder("std::vector<" + inner.getName() + ">", headers).build();
    }

    @Override
    public Symbol mapShape(MapShape shape) {
      Symbol inner = elementSymbol(shape.getValue(), shape.hasTrait(SparseTrait.class));
      Set<String> headers = new TreeSet<>(headersOf(inner));
      headers.add("<map>");
      headers.add("<string>");
      return builder("std::map<std::string, " + inner.getName() + ">", headers).build();
    }

    @Override
    public Symbol structureShape(StructureShape shape) {
      if (shape.getId().toString().equals("smithy.api#Unit")) {
        return builder("smithy::Unit", Set.of("\"smithy/core/outcome.h\"")).build();
      }
      return declared(shape);
    }

    @Override
    public Symbol unionShape(UnionShape shape) {
      return declared(shape);
    }

    @Override
    public Symbol serviceShape(ServiceShape shape) {
      return declared(shape);
    }

    @Override
    public Symbol operationShape(OperationShape shape) {
      return declared(shape);
    }

    @Override
    public Symbol resourceShape(ResourceShape shape) {
      return declared(shape);
    }

    private Symbol elementSymbol(MemberShape member, boolean sparse) {
      Symbol target = toSymbol(model.expectShape(member.getTarget()));
      if (!sparse) {
        return target;
      }
      Set<String> headers = new TreeSet<>(headersOf(target));
      headers.add("<optional>");
      return builder("std::optional<" + target.getName() + ">", headers).build();
    }
  }
}
