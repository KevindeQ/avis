from typing import Any

from data_trace.formats.property_bag import PropertyBag


class DataCollector:
    def __init__(self):
        self._container: dict[Any, list[PropertyBag]] = {}

    def add_property_bag(self, collection_id: Any, property_bag: PropertyBag):
        if collection_id not in self._container:
            self._container[collection_id] = [property_bag]

        self._container[collection_id].append(property_bag)

    def find_by_id(self, collection_id: Any) -> list[PropertyBag]:
        return self._container.get(collection_id, [])
