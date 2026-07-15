"""Analysis-time attribute validation for the smithy_cpp_*_library rules.

The accepted grammar mirrors the generator's own validation
(codegen/smithy-cpp-codegen/src/main/java/io/smithycpp/codegen/CppSettings.java);
a change to what the generator accepts must land in both places. Catching the
wiring mistakes here means the fix reaches the consumer at analysis time,
before the generator action ever runs.
"""

visibility(["//bazel/..."])

def _is_identifier(s):
    """True if s is a C/C++-style identifier ([A-Za-z_][A-Za-z0-9_]*)."""
    if not s or s[0].isdigit():
        return False
    for ch in s.elems():
        if ch != "_" and not ch.isalnum():
            return False
    return True

def namespace_error(namespace):
    """Validates a smithy_cpp_*_library `namespace` attribute.

    Args:
        namespace: the attribute value, e.g. "acme::todo".

    Returns:
        An actionable error message, or None if the value is a valid
        "::"-separated C++ namespace.
    """
    bad = [s for s in namespace.split("::") if not _is_identifier(s)]
    if not bad:
        return None
    msg = 'namespace = "%s" is not a "::"-separated C++ namespace (like "acme::todo")' % namespace
    if "." in namespace and "::" not in namespace:
        # The classic slip: pasting the model's Smithy namespace verbatim.
        return msg + ': did you mean "%s"?' % namespace.replace(".", "::")
    return msg + ': segment "%s" is not a C++ identifier' % bad[0]

def service_error(service):
    """Validates a smithy_cpp_*_library `service` attribute.

    Args:
        service: the attribute value, e.g. "acme.todo#Todo".

    Returns:
        An actionable error message, or None if the value is a valid
        absolute Smithy shape ID.
    """
    shape_ns, _, name = service.partition("#")
    well_formed = service.count("#") == 1
    if (well_formed and "::" not in shape_ns and _is_identifier(name) and
        not [s for s in shape_ns.split(".") if not _is_identifier(s)]):
        return None
    msg = 'service = "%s" is not the service\'s Smithy shape ID' % service
    if well_formed and "::" in shape_ns:
        # The reverse slip: the C++ namespace attribute pasted into service.
        return msg + ': did you mean "%s#%s"?' % (shape_ns.replace("::", "."), name)
    return msg + ' (expected "<namespace>#<ServiceName>" exactly as modeled, like "acme.todo#Todo")'
