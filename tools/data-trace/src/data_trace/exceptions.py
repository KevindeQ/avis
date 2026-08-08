class ConfigurationError(RuntimeError):
    """
    Raised when an error is encountered in the configuration
    """


class UnknownFormat(RuntimeError):
    """
    Raised when an unknown input or output format is encountered
    """
