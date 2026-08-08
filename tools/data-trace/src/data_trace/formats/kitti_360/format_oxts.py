from pathlib import Path

from data_trace.formats.format import FileReader, Format
from data_trace.formats.property_bag import PropertyBag


class OxtsFormat(Format, FileReader):
    @staticmethod
    def name() -> str:
        return "oxts"

    def read(self, path: Path) -> PropertyBag:
        if not path.exists():
            raise FileNotFoundError(f"Could not find file \"{path.name}\"")

        # Read complete file into memory
        with open(path, "r") as file:
            contents = file.read()

        data_blocks = contents.split(" ")
        if len(data_blocks) < 30:
            raise RuntimeError(f"File \"{path.name}\" has an invalid file format. It contains not enough data blocks.")
        elif len(data_blocks) > 30:
            raise RuntimeError(f"File \"{path.name}\" has an invalid file format. It contains too many data blocks.")

        property_bag = PropertyBag()
        property_bag.lat = float(data_blocks[0])
        property_bag.lon = float(data_blocks[1])
        property_bag.alt = float(data_blocks[2])
        property_bag.roll = float(data_blocks[3])
        property_bag.pitch = float(data_blocks[4])
        property_bag.yaw = float(data_blocks[5])
        property_bag.velocity_north = float(data_blocks[6])
        property_bag.velocity_east = float(data_blocks[7])
        property_bag.velocity_forward = float(data_blocks[8])
        property_bag.velocity_left = float(data_blocks[9])
        property_bag.velocity_up = float(data_blocks[10])
        property_bag.acceleration_x = float(data_blocks[11])
        property_bag.acceleration_y = float(data_blocks[12])
        property_bag.acceleration_z = float(data_blocks[13])
        property_bag.acceleration_forward = float(data_blocks[14])
        property_bag.acceleration_left = float(data_blocks[15])
        property_bag.acceleration_up = float(data_blocks[16])
        property_bag.angular_rate_x = float(data_blocks[17])
        property_bag.angular_rate_y = float(data_blocks[18])
        property_bag.angular_rate_z = float(data_blocks[19])
        property_bag.angular_rate_forward = float(data_blocks[20])
        property_bag.angular_rate_left = float(data_blocks[21])
        property_bag.angular_rate_up = float(data_blocks[22])
        property_bag.accuracy_position = float(data_blocks[23])
        property_bag.accuracy_velocity = float(data_blocks[24])

        property_bag.navigation_status = int(data_blocks[25])
        property_bag.number_of_satellites = int(data_blocks[26])
        property_bag.position_mode = int(data_blocks[27])
        property_bag.velocity_mode = int(data_blocks[28])
        property_bag.orientation_mode = int(data_blocks[29])

        return property_bag
