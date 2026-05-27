"""
CANopen SDO read/write example.

Demonstrates:
  - cangaroo.sdo_read()   — read an object value via expedited or segmented/domain SDO upload
  - cangaroo.sdo_write()  — write an object value via expedited SDO download

Adjust INTERFACE_ID, NODE_ID, INDEX, and SUB_INDEX for your device.
"""
import cangaroo

INTERFACE_ID = 0
NODE_ID = 1
INDEX = 0x2000
SUB_INDEX = 0x00

print("Available interfaces:")
for iface in cangaroo.interfaces():
    print(f"  id={iface['id']}  {iface['name']} ({iface['bus_type']})")

print(f"\nReading 0x{INDEX:04X}:{SUB_INDEX:02X} from node {NODE_ID}...")
value = cangaroo.sdo_read(
    node_id=NODE_ID,
    index=INDEX,
    sub_index=SUB_INDEX,
    interface_id=INTERFACE_ID,
    timeout=1.0,
)
print(
    f"Read ok: raw=0x{value['raw']:X}, size={value['size']} bytes, "
    f"data={value['data'].hex(' ')}"
)

if value["size"] > 4:
    print("Domain/segmented upload detected.")

if value["size"] <= 4:
    new_value = value["raw"]
    print(f"\nWriting back same value 0x{new_value:X}...")
    result = cangaroo.sdo_write(
        node_id=NODE_ID,
        index=INDEX,
        sub_index=SUB_INDEX,
        value=new_value,
        size=value["size"],
        interface_id=INTERFACE_ID,
        timeout=1.0,
    )
    print(f"Write ok: {result['ok']}")
else:
    print("\nSkipping write-back for domain/segmented upload values larger than 4 bytes.")
