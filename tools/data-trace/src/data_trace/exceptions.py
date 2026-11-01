class ConfigurationError(RuntimeError):
    """
    Raised when an error is encountered in the configuration
    """


class ShaderManifestError(RuntimeError):
    """
    Raised when a shader list, a reflection file or a shader's bindings can't be turned into a manifest
    """


class UnknownFormat(RuntimeError):
    """
    Raised when an unknown input or output format is encountered
    """
