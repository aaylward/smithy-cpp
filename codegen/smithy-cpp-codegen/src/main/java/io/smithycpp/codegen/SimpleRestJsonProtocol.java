package io.smithycpp.codegen;

import software.amazon.smithy.model.shapes.ShapeId;

/**
 * alloy#simpleRestJson: the vendor-neutral HTTP + JSON protocol used by smithy4s. Adopted from
 * alloy-core; the recommended REST protocol for 0.1.0. It shares the entire HTTP-binding model of
 * {@link RestJson1Protocol}; the only wire difference is error identity — modeled errors are
 * discriminated by the neutral {@code X-Error-Type} response header (the error shape name), with
 * status-code fallback, rather than the AWS {@code X-Amzn-Errortype} header.
 */
final class SimpleRestJsonProtocol extends RestJson1Protocol {

  static final ShapeId TRAIT = ShapeId.from("alloy#simpleRestJson");

  @Override
  protected String errorTypeHeaderName() {
    return "x-error-type";
  }

  @Override
  public String name() {
    return "simpleRestJson";
  }

  @Override
  public ShapeId traitId() {
    return TRAIT;
  }
}
