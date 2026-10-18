# 04 — Scenarios & KITTI-360 Data

## Code placement

| Feature | Namespace | CMake target | Location |
|---|---|---|---|
| Scenario model, YAML load/save, validation | `avis::simulation` | `AVIS_LIB` | `include/avis/simulation/scenario*.h`, `lib/avis/simulation/` |
| Sequence-pack readers (`stream_reader`, `box_reader`, `tile_reader`, manifest), used only by the Driver | `avis::dataset` | `AVIS_LIB` | `include/avis/dataset/`, `lib/avis/dataset/` |
| Scenario editing (undo commands, editor UI) | `avis::visualizer` | `AVIS_VISUALIZER` | `apps/visualizer/ui/commands/`, `apps/visualizer/simulation/scenario_model.*` |
| Packer tool | Python package `kitti360_pack` | — (uv project, not CMake) | `tools/kitti360-pack/` |

## 1. Overview

A **scenario** is a YAML file that fully describes one simulation run:
- which data source (KITTI-360 sequence and frame range) to replay
- which input sensors to feed into the Driver
- which perception module and vehicle model to load, with their parameters
- the simulation timestep and random seed
- Visualizer-only display settings (lidar point display, environment lighting, camera)

Editing a simulation run means **selecting the input sensors and the data** they come from, plus the plug-ins and parameters. The scene itself isn't authored: it starts empty and is built by the Driver during the run ([02 §5.8](02-communication.md#58-scene-stream)).

KITTI-360 is used in **open-loop replay**: the ego vehicle follows its recorded trajectory, and the Driver's vehicle model drives a separate **shadow vehicle** (see [05](05-driver.md)).

Raw KITTI-360 is converted once by a Python tool into **sequence packs**. These are compact, random-access, timestamp-indexed files that **only the Driver** reads. The Visualizer learns what's available from the Driver's `describe` reply ([02 §5.2](02-communication.md#52-describe)).

## 2. Scenario file

### 2.1 Location and naming
- The extension is `*.avis.yaml`. Scenarios can live anywhere. The Visualizer remembers recent files.
- Scenarios contain **no absolute dataset paths**. They name a dataset kind and a sequence, and the Driver resolves these through its own configuration ([01 §7](01-system-architecture.md#7-configuration)). That keeps scenarios portable between machines and users.

### 2.2 Schema (version 1)

```yaml
avis_scenario: 1                       # schema version (required)
name: "Drive 0000 – Junction left turn"
description: >
  Ego approaches a junction; cyclist crosses from the right.

simulation:
  timestep: 20ms                       # duration literal: ns|us|ms|s; 1ms..100ms
  random_seed: 42                      # u64
  time_scale: 1.0                      # initial speed; UI can change it
                                       # the run always ends in `stopped` at end of data (03 §3);
                                       # failure detection is per machine (visualizer.yaml → lockstep, 02 §6.2)

dataset:                               # picked from the Driver's data catalogue (02 §5.2)
  kind: kitti360
  sequence: 2013_05_28_drive_0000_sync
  frames: { start: 250, end: 1450 }    # inclusive KITTI frame indices; the only way to select the range
  sensors:                             # input sensors replayed to the Driver
    - image_00                         # left perspective camera (rectified)
    - image_01                         # right perspective camera (rectified)
    - velodyne                         # HDL-64E lidar
    - oxts                             # GNSS/IMU
    # image_02, image_03 (fisheye) and sick are available but off by default

driver:
  id: driver-0                         # which Driver instance to bind
  perception:
    module: ground_truth_oracle
    parameters:                        # free-form, interpreted by the module
      maximum_range: 80.0
      minimum_lidar_points: 10
      classes: [car, truck, bus, bicycle, motorcycle, pedestrian, rider]
      noise: { position_sigma: 0.15, yaw_sigma: 0.02, dropout: 0.02 }
  vehicle_model:
    module: example_lane_follower      # plug-in name (DLL)
    parameters: { target_speed: 8.0 }
  shadow_vehicle:
    enabled: true
    dynamics: kinematic_bicycle        # kinematic_bicycle | jolt_wheeled
    resync_interval: 10s               # 0 = never re-synchronization to recorded pose
    parameters: { wheelbase: 2.71, maximum_steer: 0.61, length: 4.6, width: 1.8, height: 1.5 }

visualization:                         # Visualizer only, never sent to the Driver
  lidar:
    color_mode: intensity              # intensity | height | ring | uniform
    point_size: 0.05                   # metres (min 1 px on screen)
  environment:
    sun: { azimuth: 135deg, elevation: 45deg, illuminance: 100000 }   # lux
    sky: procedural
  camera:
    mode: follow                       # follow | free_fly
    follow: { offset: [-8.0, 0.0, 3.0], lag: 0.3s }
```

The scenario holds **no entities**. The ego vehicle, its sensor rig, perceived objects and the vehicle model's belief are created by the Driver during the run and arrive through the scene stream. The inspector shows them read-only.

### 2.3 Validation
- Parsing uses yaml-cpp into strongly typed structs (`avis::simulation::scenario`). Every error reports `file:line:column` and a path such as `driver.shadow_vehicle.parameters.wheelbase`.
- Unknown keys produce **warnings**, not errors, for forward compatibility.
- Local checks (always): `timestep` is a whole number of ns within range, required sections are present, and the frame range is ordered.
- The data range is always `dataset.frames`: frame indices are the exact key into the dataset. Seconds are only a display and input aid in the editor ([12 §6](12-visualizer.md#6-panels-and-controls)). A `time:` range, an `end_behaviour` or a `step_timeout` key from an older draft is reported as an unknown key (a warning) and ignored.
- Catalogue checks (when a Driver is connected): the sequence exists, the frame range lies within it, the selected sensors exist, the plug-ins are available, and the sequence has the **capabilities** the selected modules need (`boxes` for `ground_truth_oracle`, `semantics` for `jolt_wheeled`, a warning for `kinematic_bicycle` without `semantics`, §3.1), all against the `describe` reply ([02 §5.2](02-communication.md#52-describe)).
- Parameter checks (when the module publishes a `parameters_schema`): the Visualizer checks `driver.perception.parameters`, `driver.vehicle_model.parameters` and `driver.shadow_vehicle.parameters` against their module's schema with the same rules as the Driver ([05 §5.8](05-driver.md#58-parameter-schema)): type and range errors are errors, unknown keys are warnings.
- The Driver re-validates on `load_scenario` and is the final authority (for example, missing poses in the frame range, or a plug-in rebuilt with a changed schema since the editor last rescanned). Its diagnostics join the same problems list.
- Validation returns `result<scenario, std::vector<diagnostic>>`. All problems are collected and reported, not just the first.

### 2.4 Editing rules
- Editing is allowed **only in the `stopped` state** ([03 §3](03-simulation-core.md#3-clock-state-machine)). The UI disables editors in every other state, and the scenario model rejects mutations with an error as a second line of defence.
- Edits go through a `QUndoStack` of commands (set a scenario value, select or deselect a sensor, change the data source or a plug-in). They apply to the in-memory scenario model. There's no scene to preview while stopped, since the scene comes from the Driver.
- **Save** serialises the scenario model back to YAML in canonical key order. yaml-cpp doesn't keep comments or the original formatting, so the file is rewritten in canonical form. Losing comments is accepted (decision D-07). The status bar shows a one-time notice when a file that contained comments is saved.
- *Save As*, *Revert* and a dirty indicator are provided. Autosave writes to `<name>.avis.yaml.autosave` every 2 minutes while dirty.

## 3. KITTI-360 dataset

### 3.1 What is used

| KITTI-360 asset | Used by | Purpose |
|---|---|---|
| `data_2d_raw/<sequence>/image_00, image_01` (rectified perspective, 1408×376) | Driver | Camera sensor stream |
| `data_2d_raw/<sequence>/image_02, image_03` (fisheye, 1400×1400) | Driver (optional) | Camera sensor stream |
| `data_3d_raw/<sequence>/velodyne_points` (HDL-64E, ~10 Hz) | Driver | Lidar sensor stream |
| `data_3d_raw/<sequence>/sick_points` | Driver (optional) | Push-broom lidar |
| `data_poses/<sequence>/oxts` | Driver | GNSS/IMU sensor stream |
| `data_poses/<sequence>/poses.txt` | Driver | Ground-truth ego pose (IMU → world): the published ego vehicle pose in open loop |
| `calibration/*` | Driver | Intrinsics and extrinsics |
| `data_3d_bboxes/train/<sequence>.xml` | Driver (oracle) | Static and dynamic 3D boxes |
| `data_3d_semantics/train/<sequence>/static/*.ply` | Driver | Accumulated static point cloud (RGB + semantic label), used for the Jolt ground height field |

KITTI-360 doesn't annotate animals. The `animal` class exists in the Avis class enum for future datasets but is never produced from KITTI-360.

**Train and test split.** 3D boxes and semantic point clouds are released only for the **training** sequences. The test sequences (0008 and 0018, to be confirmed when packing) have sensor data and poses but no annotations. The packer records what a sequence has as **capabilities** (§4.7), and the Driver reports them in its catalogue ([02 §5.2](02-communication.md#52-describe)):

| Capability | Source | Needed by | Without it |
|---|---|---|---|
| `boxes` | `data_3d_bboxes` | `ground_truth_oracle` | Loading the oracle is an **error**. Learned perception plug-ins still work. |
| `semantics` | `data_3d_semantics` (→ ground tiles, §4.6a) | `jolt_wheeled` shadow dynamics | Loading `jolt_wheeled` is an **error**. |
| `semantics` | (same) | `kinematic_bicycle` ground height | **Warning**: the ground height falls back to the recorded ego height (nearest pose, minus the vehicle-frame offset), and the Jolt collision world has no ground. |

### 3.2 Class mapping

Classes are used inside the Driver and in the plug-in ABI. The Visualizer receives them only as text properties (for example `perception.classification`).

| Avis class | KITTI-360 labels |
|---|---|
| `car` | car |
| `truck` | truck |
| `bus` | bus |
| `bicycle` | bicycle |
| `motorcycle` | motorcycle |
| `pedestrian` | person |
| `rider` | rider |
| `trailer` | trailer, caravan |
| `other_vehicle` | train, unknown vehicle |
| `static_structure` | building, pole, traffic sign, traffic light, … (static boxes, rendered only if enabled) |
| `animal` | — (not available) |

## 4. Sequence pack format

### 4.1 Layout

```
<kitti360-pack-root>/
  2013_05_28_drive_0000_sync/
    manifest.yaml            # metadata + per-file xxh3 hashes + manifest content hash
    calibration.yaml         # all sensor extrinsics in vehicle frame + intrinsics (§4.8)
    frames.avs               # reference timeline: image_00 timestamp + has_pose per frame index (§4.2a)
    poses.avs                # ground-truth poses, timestamped with the reference timeline
    oxts.avs                 # GNSS/IMU records
    image_00.avs             # perspective camera records (encoded PNG bytes as in source)
    image_01.avs
    image_02.avs             # fisheye camera records (PNG), MEI model (§4.8)
    image_03.avs
    velodyne.avs             # lidar sweeps
    sick.avs                 # SICK line scans converted to 3D points (lidar_xyzi)
    boxes.avb                # static objects + dynamic object instances (capability `boxes`)
    pointcloud/              # capability `semantics`
      tiles.avt              # tile index
      tile_<index_x>_<index_y>.avp     # per-tile progressive point data
    ground/                  # capability `semantics`
      ground.avt             # ground tile index
      ground_<index_x>_<index_y>.avg   # per-tile height grid (§4.6a)
```

### 4.2 Stream file (`.avs`)
A generic, memory-mappable, timestamp-indexed record container.

| Section | Contents |
|---|---|
| Header (64 B) | `magic 'AVS1'`, `version u16`, `stream_kind u16` (pose, oxts, image_png, lidar_xyzi, …), `record_count u32`, `index_offset u64`, `schema_hash u64`, `time_base_ns i64` (absolute ns of the first record), reserved |
| Records | Concatenated, each 16-byte aligned |
| Index | `record_count ×` { `timestamp_ns i64` (relative to `time_base_ns`), `frame_index u32`, `size u32`, `offset u64`, `crc32 u32` (CRC-32/IEEE of the record bytes, [13 §4](13-build-and-quality.md#4-dependency-management)), `padding u32` } sorted by timestamp |

All streams of a pack share one `time_base_ns`: the reference timestamp of frame 0 (§4.2a).

Record schemas:
- **frame**: `u8 has_pose`, padding. The record's index timestamp is the reference timestamp of that frame index (§4.2a).
- **pose**: `f64[12]` row-major 3×4 transform, vehicle (IMU) → world, timestamped with the reference timestamp of its frame index (`poses.txt` itself has no timestamps).
- **oxts**: the 30 OXTS fields in KITTI order: `lat, lon, alt, roll, pitch, yaw, vn, ve, vf, vl, vu, ax, ay, az, af, al, au, wx, wy, wz, wf, wl, wu, pos_accuracy, vel_accuracy` as `f64`, then `navstat, numsats, posmode, velmode, orimode` as `i32`. The same layout is `avis_oxts_record` in the plug-in ABI ([05 §5.3](05-driver.md#53-c-interface-avis_pluginh-abridged)).
- **image_png**: the original PNG bytes. The Driver decodes them with WIC, so packing has no lossy step.
- **lidar_xyzi**: `u32 point_count`, then `f32[4]` per point in the sensor frame. The SICK stream uses the same schema: the packer converts each 2D line scan to 3D points in the SICK sensor frame, with intensity 0 where the source has none. The exact SICK source record format is to be checked against the dataset when the packer is written.

Every stream keeps its **own** timestamps from its `timestamps.txt`. Lookup by time is a binary search over the index. The Driver's replay asks for every record with a timestamp in `(t_{k-1}, t_k]`.

### 4.2a Reference timeline (`frames.avs`)
KITTI-360's synchronised data shares a **frame index** across sensors, but each sensor's timestamps differ by a few milliseconds (the cameras are triggered by the rotating lidar), and `poses.txt` has only frame indices. A single reference clock is needed to define simulation time:

- **The reference timestamp of a frame is its `image_00` timestamp.** `image_00` is the canonical KITTI-360 frame clock, and the poses are indexed by it. `frames.avs` holds one record per frame index in the sequence. It's always packed, even if a scenario disables `image_00`.
- **t = 0** is the reference timestamp of `frames.start`. **`t_end`** is the reference timestamp of `frames.end`. All stream timestamps are measured from the same zero, so a velodyne sweep 3 ms before frame `start`'s camera image has `t = −3 ms`.
- **Steps:** step `k` covers `(t_{k−1}, t_k]` with `t_k = k · Δt`. **Step 0 covers `(−Δt, 0]`**, so the data at exactly t = 0 is delivered at step 0 ([02 §6.2](02-communication.md#62-rules)). Samples before `−Δt` are never delivered.
- **`step_count = ceil(t_end / Δt) + 1`**: steps 0 to `K = ceil(t_end / Δt)`, with `t_K ≥ t_end`. Step K returns `end_of_data` ([02 §5.4](02-communication.md#54-load_scenario)).
- A frame range is valid only if both `frames.start` and `frames.end` have a reference record. Dropped frames inside the range are fine: the timeline simply has a larger gap there.

### 4.3 Box file (`.avb`)
- A static object table: `object_id u32`, `class u8`, `semantic_id u16`, `instance_id u32`, `center f64x3`, `extent f32x3`, `rotation f32x4` (quaternion), and the 8 corner vertices for exact rendering.
- Dynamic instance stream: an index of `(timestamp_ns, first_instance, count)` per frame, plus instance records `object_id`, `center`, `extent`, `rotation`.
- Object ids are stable per sequence. The Driver uses them as source-local entity ids ([02 §5.7](02-communication.md#57-common-types)), which keeps ordering deterministic.

### 4.4 Coordinate frames and precision
- KITTI-360 world coordinates are large (hundreds of metres to kilometres). Packs store them in `f64`.
- At scenario load, the Driver picks the **scenario origin** `O`, the ego position at `frames.start`, and returns it in the `load_scenario` reply ([02 §5.4](02-communication.md#54-load_scenario)). The Driver works in `f64` world coordinates, and messages carry `f64` world positions ([02 §5.7](02-communication.md#57-common-types)). Every Visualizer-side position is `f32(p − O)`, and so is every position inside the Driver's Jolt world ([05 §3.3](05-driver.md#33-shadow-vehicle)).
- Calibration is re-expressed in the Avis vehicle frame (ISO 8855, origin at the rear axle on the ground). The fixed vehicle-to-IMU offset is a pack constant derived from the KITTI-360 sensor layout.

### 4.5 Ego pose at simulation time
The Driver publishes the ego vehicle at `pose(t_k)` (open loop: the vehicle is where it was recorded). `pose(t)` interpolates between the two pose records around simulation time `t`, using their reference timestamps (§4.2a). Position uses linear interpolation in `f64`, rotation uses slerp. Frames without poses (KITTI-360 has gaps) are bridged by interpolation, and gaps over 0.5 s produce a load-time warning.

### 4.6 Point cloud tiles (`.avt`, `.avp`)
- The accumulated static `.ply` windows are merged and deduplicated on a 2 cm voxel grid, then split into **25.6 m × 25.6 m XY tiles**.
- Each point is 12 bytes: `u16x3` position quantised within the tile's AABB (about 0.4 mm on XY), `u8x3` RGB, `u8` semantic label, `u8x2` octahedral normal (estimated offline from 16 nearest neighbours, oriented upward or towards the sensor trajectory).
- The points in a tile are written in **random (progressive) order**, so any prefix is a uniform subsample.
- The packer derives the ground tiles (§4.6a) from these points. The Driver doesn't read the point tiles at run time, and they aren't sent to the Visualizer: what the Visualizer shows of the world is what the vehicle model publishes as belief geometry. They're kept in the pack for plug-ins and tools (for example `avis-pack-inspect`), and for a future world-rendering option.
- `tiles.avt` holds, per tile, the AABB, point count, file name and xxh3 hash.

### 4.6a Ground tiles (`.avt`, `.avg`)
The shadow vehicle's ground ([05 §3.3](05-driver.md#33-shadow-vehicle)) is built **offline by the packer**, so the Driver only loads small grids.

- **Source points:** the merged semantic points (§4.6) with a ground-like label: `road`, `sidewalk`, `parking`, `ground`, `terrain`.
- **Grid:** per 25.6 m × 25.6 m tile (aligned with the point tiles), a height grid at **0.2 m** resolution, in `f32` metres relative to the tile's AABB minimum `z` (stored as `f64` in the header). Neighbouring tiles **share one edge row and column** of samples, so there are no cracks. The exact sample count per side (about 128 plus the shared edge) is chosen to meet Jolt's height-field block-size rules; to be checked when implementing.
- **Height per cell:** the **median** height of the ground points in the cell. That's robust against stray curb and vegetation points.
- **Holes:** filled by iterative diffusion from neighbouring cells, up to **2 m** from the nearest measured cell. Cells further away are marked **no ground** with Jolt's no-collision sample value, so nothing is invented far from the road.
- **Limits:** a height field is 2.5D, so bridges and overpasses can't be represented. That's acceptable for KITTI-360's suburban routes.
- `ground.avt` holds, per tile, the tile index, AABB, sample count, file name and xxh3 hash. Each `.avg` holds a small header (origin, resolution, sample count) and the `f32` samples.

### 4.7 Manifest
```yaml
avis_pack: 1
dataset: kitti360
sequence: 2013_05_28_drive_0000_sync
source_version: "KITTI-360 v1.0"
packer_version: "0.1.0"
time_base_ns: 1369730762783950352     # reference timestamp of frame 0 (§4.2a)
frames: { first: 0, last: 11517 }
split: train                           # train | test
capabilities: [boxes, semantics]       # what this sequence has (§3.1); empty for test sequences
streams:
  frames:   { file: frames.avs,   records: 11518, xxh3: "c41e..." }
  image_00: { file: image_00.avs, records: 11518, rate_hz: 10.0, xxh3: "9f1c..." }
  velodyne: { file: velodyne.avs, records: 11518, rate_hz: 10.0, xxh3: "07ab..." }
  ...
ground: { file: ground/ground.avt, tiles: 412, resolution_m: 0.2, xxh3: "5d20..." }
content_hash: "3b5e0a7c91d24f60"      # xxh3 over the sorted list of file hashes
```
`content_hash` identifies the pack version. The Driver writes it into the determinism hash file (`--record-hashes`), so two runs can be checked to have used the same data.

### 4.8 Calibration (`calibration.yaml`)
All extrinsics are expressed in the **Avis vehicle frame** (ISO 8855, rear-axle centre on the ground, [00 §4.1](00-overview.md#41-coordinate-systems-and-units)). Matrices are row-major 4×4.

```yaml
avis_calibration: 1
imu_to_vehicle: [ ... 16 values ... ]   # pack constant from the KITTI-360 sensor layout (§4.4)
sensors:
  image_00:
    kind: camera
    sensor_to_vehicle: [ ... ]
    image: { width: 1408, height: 376 }
    model: pinhole                       # rectified perspective, no distortion
    pinhole: { focal_length_x: 552.55, focal_length_y: 552.55, principal_point_x: 682.05, principal_point_y: 238.77 }
  image_02:
    kind: camera
    sensor_to_vehicle: [ ... ]
    image: { width: 1400, height: 1400 }
    model: mei                           # MEI unified camera model (fisheye)
    mei: { xi: 2.21, k1: 0.017, k2: 1.65, p1: 0.0, p2: 0.0, gamma1: 1336.9, gamma2: 1336.4, u0: 716.9, v0: 705.7 }
  velodyne: { kind: lidar,    sensor_to_vehicle: [ ... ] }
  sick:     { kind: lidar,    sensor_to_vehicle: [ ... ] }
  oxts:     { kind: gnss_imu, sensor_to_vehicle: [ ... ] }
```
(The numbers are illustrative; the packer copies them from the dataset.)

- **Camera models:** `pinhole` for the rectified perspective cameras (`image_00/01`, from `perspective.txt`), and `mei` for the fisheye cameras (`image_02/03`, from `image_0{2,3}.yaml`). The plug-in ABI carries both in `avis_camera_calibration` ([05 §5.3](05-driver.md#53-c-interface-avis_pluginh-abridged)).
- The packer derives the extrinsics by chaining `calib_cam_to_pose.txt`, `calib_cam_to_velo.txt` and `calib_sick_to_velo.txt` with `imu_to_vehicle`.
- The Driver loads this file at `load_scenario`, and hands each enabled sensor's calibration to plug-ins (`avis_dataset_information` and with every sample).

## 5. Packer tool: `tools/kitti360-pack`

A Python package managed with `uv`, structured like `tools/data-trace`.

```bash
uv run kitti360-pack --raw E:/datasets/KITTI-360 --out E:/datasets/kitti360-avis \
    --sequence 2013_05_28_drive_0000_sync [--streams image_00,image_01,velodyne,oxts] [--no-pointcloud]
```

| Step | Notes |
|---|---|
| Parse timestamps | Per-sensor `timestamps.txt` → int64 ns |
| Reference timeline | `image_00` timestamps per frame index, plus `has_pose` from `poses.txt` → `frames.avs`; `time_base_ns` = the reference timestamp of frame 0 (§4.2a) |
| Write streams | Images (perspective and fisheye) are copied byte-for-byte. Lidar `.bin` is wrapped with a point count. SICK scans are converted to 3D `lidar_xyzi`. OXTS text is parsed to binary. Poses get their frame's reference timestamp. |
| Calibration | Parse `calib_cam_to_pose.txt`, `calib_cam_to_velo.txt`, `perspective.txt`, `image_0{2,3}.yaml` (MEI), `calib_sick_to_velo.txt` and write `calibration.yaml` in the vehicle frame (§4.8) |
| Split and capabilities | Detect whether boxes and semantics exist for the sequence; write `split` and `capabilities` (§3.1) |
| Boxes (`boxes` only) | Parse the XML with `lxml`, split static and dynamic, map classes |
| Point cloud (`semantics` only) | Read `.ply` with `plyfile`, voxel-dedupe and estimate normals with `numpy` and `scipy.spatial.cKDTree`, then tile and shuffle with a fixed seed so output is reproducible |
| Ground tiles (`semantics` only) | Grid the ground-labelled points per tile (median per 0.2 m cell), fill holes by diffusion up to 2 m, mark the rest no-ground, write `ground/` (§4.6a) |
| Manifest | Compute xxh3 hashes (`xxhash` package) and write the manifest |

Dependencies: `numpy`, `scipy`, `plyfile`, `lxml`, `pyyaml`, `xxhash`. The tool is idempotent and can resume (it skips files whose hashes already match). Tests use a tiny synthetic sequence fixture.

A companion C++ reader lives in `include/avis/dataset/` (`avis::dataset::stream_reader`, `box_reader`, `tile_reader`, `ground_reader`, `calibration`, `manifest`). Only the Driver uses it. It is built on the IO layer ([07](07-io-layer.md)), using memory mapping for indices and async reads for records.
