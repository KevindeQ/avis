from typing import Any


class PropertyBag:
    def __init__(self):
        self._properties: dict[str, Any] = {}

    def __getattr__(self, name) -> Any:
        return self._properties[name]

    def __setattr__(self, name: str, value: Any):
        if name.startswith("_"):
            super().__setattr__(name, value)
        else:
            self._properties[name] = value
