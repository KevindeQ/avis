# Invalid glTF fixtures

Hand-written for `tests/test_scene_gltf.cpp`, under the same licence as the Avis sources.

- `RequiredUnknownExtension.gltf` requires `EXT_avis_unknown`: the import fails with `gltf_unsupported`.
- `BrokenAccessor.gltf` has a `POSITION` accessor of three `VEC3`s over a 12-byte buffer view: `gltf_invalid`.
