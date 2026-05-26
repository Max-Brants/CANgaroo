#include <iostream>
#include <iomanip>
#include <cassert>
#include <QFile>
#include <QTextStream>
#include "decoders/UdsDecoder.h"
#include "decoders/J1939Decoder.h"
#include "decoders/CanOpenDecoder.h"
#include "core/DBC/CanOpenDb.h"

void testUdsSingleFrame() {
    UdsDecoder decoder;
    BusMessage msg(0x7E0);
    msg.setLength(8);
    msg.setByte(0, 0x02); // SF, size 2
    msg.setByte(1, 0x10); // SID 0x10
    msg.setByte(2, 0x01); // Param 0x01
    
    ProtocolMessage out;
    DecodeStatus result = decoder.tryDecode(msg, out);
    assert(result == DecodeStatus::Completed);
    assert(out.name == "DiagnosticSessionControl");
    assert(out.protocol == "uds");
    assert(out.id == 0x10);
    assert(out.type == MessageType::Request);
    assert(out.payload.size() == 2);
    assert(static_cast<uint8_t>(out.payload[0]) == 0x10);
    std::cout << "testUdsSingleFrame passed" << std::endl;
}

void testUdsMultiFrame() {
    UdsDecoder decoder;
    ProtocolMessage out;

    // FF
    BusMessage ff(0x7E0);
    ff.setLength(8);
    ff.setByte(0, 0x10); // FF
    ff.setByte(1, 0x0A); // Size 10
    ff.setByte(2, 0x22); // SID 0x22
    for(int i=3; i<8; i++) ff.setByte(i, i);
    
    assert(decoder.tryDecode(ff, out) == DecodeStatus::Consumed);

    // CF
    BusMessage cf(0x7E0);
    cf.setLength(8);
    cf.setByte(0, 0x21); // CF, SN 1
    for(int i=1; i<6; i++) cf.setByte(i, 0xA0 + i);
    
    DecodeStatus result = decoder.tryDecode(cf, out);
    assert(result == DecodeStatus::Completed);
    assert(out.name == "ReadDataByIdentifier");
    assert(out.payload.size() == 10);
    assert(out.id == 0x22);
    assert(out.type == MessageType::Request);
    std::cout << "testUdsMultiFrame passed" << std::endl;
}

void testJ1939SingleFrame() {
    J1939Decoder decoder;
    BusMessage msg(0x18FEEF01); // PGN 65263 (EngTemp), SA 1, Pri 6
    msg.setExtended(true);
    msg.setLength(8);
    for(int i=0; i<8; i++) msg.setByte(i, i);

    ProtocolMessage out;
    DecodeStatus result = decoder.tryDecode(msg, out);
    assert(result == DecodeStatus::Completed);
    assert(out.name == "Engine Fluid Level/Pressure");
    assert(out.protocol == "J1939");
    assert(out.id == 0xFEEF);
    assert(out.type == MessageType::Request);
    std::cout << "testJ1939SingleFrame passed" << std::endl;
}

void testUdsNegativeResponse() {
    UdsDecoder decoder;
    BusMessage msg(0x7E8);
    msg.setLength(8);
    msg.setByte(0, 0x03); // SF
    msg.setByte(1, 0x7F); // SID 0x7F (Negative Response)
    msg.setByte(2, 0x22); // SID being rejected
    msg.setByte(3, 0x33); // NRC 0x33 (Security Access Denied)
    
    ProtocolMessage out;
    DecodeStatus result = decoder.tryDecode(msg, out);
    assert(result == DecodeStatus::Completed);
    assert(out.type == MessageType::NegativeResponse);
    assert(out.name == "NegativeResponse");
    assert(out.description == "negative response: Security Access Denied");
    std::cout << "testUdsNegativeResponse passed" << std::endl;
}


void testCanOpenEdsLoad() {
    const QString path = QStringLiteral("/tmp/cangaroo-canopen-test.eds");
    QFile file(path);
    assert(file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
    QTextStream out(&file);
    out << "[DeviceInfo]
";
    out << "VendorName=Acme
";
    out << "ProductName=Demo Node

";
    out << "[DeviceCommissioning]
";
    out << "NodeID=0x05

";
    out << "[2000]
";
    out << "ParameterName=ControlWord
";
    out << "DataType=0x0006
";
    out << "AccessType=rw
";
    out << "PDOMapping=1
";
    out << "DefaultValue=0

";
    out << "[2001]
";
    out << "ParameterName=StatusWord
";
    out << "DataType=0x0006
";
    out << "AccessType=ro
";
    out << "PDOMapping=1
";
    out << "DefaultValue=0

";
    out << "[1400sub1]
";
    out << "ParameterName=RPDO1 COB-ID
";
    out << "DataType=0x0007
";
    out << "DefaultValue=$NODEID+0x200

";
    out << "[1600sub0]
";
    out << "DataType=0x0005
";
    out << "DefaultValue=1

";
    out << "[1600sub1]
";
    out << "DataType=0x0007
";
    out << "DefaultValue=0x20000010

";
    out << "[1800sub1]
";
    out << "ParameterName=TPDO1 COB-ID
";
    out << "DataType=0x0007
";
    out << "DefaultValue=$NODEID+0x180

";
    out << "[1A00sub0]
";
    out << "DataType=0x0005
";
    out << "DefaultValue=1

";
    out << "[1A00sub1]
";
    out << "DataType=0x0007
";
    out << "DefaultValue=0x20010010
";
    file.close();

    CanOpenDb db;
    assert(db.loadFile(path));
    assert(db.deviceName() == "Demo Node");
    assert(db.configuredNodeId() == 5);
    const CanOpenObjectEntry *controlWord = db.findObject(0x2000, 0x00);
    assert(controlWord != nullptr);
    assert(controlWord->name == "ControlWord");

    const QList<const CanOpenPdo*> tpdos = db.findMatchingPdos(0x185, 5, true);
    assert(tpdos.size() == 1);
    assert(!tpdos.first()->mappings.isEmpty());
    assert(tpdos.first()->mappings.first().objectName == "StatusWord");
    std::cout << "testCanOpenEdsLoad passed" << std::endl;
}

void testCanOpenSdoDecode() {
    CanOpenDecoder decoder;
    BusMessage msg(0x605);
    msg.setLength(8);
    msg.setByte(0, 0x2B); // expedited 2-byte download request
    msg.setByte(1, 0x00);
    msg.setByte(2, 0x20);
    msg.setByte(3, 0x00);
    msg.setByte(4, 0x34);
    msg.setByte(5, 0x12);
    msg.setByte(6, 0x00);
    msg.setByte(7, 0x00);

    ProtocolMessage out;
    DecodeStatus result = decoder.tryDecode(msg, out);
    assert(result == DecodeStatus::Completed);
    assert(out.protocol == "CANopen");
    assert(out.name == "SDO Download Request");
    assert(out.metadata.value("Node ID").toInt() == 5);
    assert(out.metadata.value("Value").toString() == "4660");
    std::cout << "testCanOpenSdoDecode passed" << std::endl;
}

int main() {
    testUdsSingleFrame();
    testUdsMultiFrame();
    testJ1939SingleFrame();
    testUdsNegativeResponse();
    testCanOpenEdsLoad();
    testCanOpenSdoDecode();
    std::cout << "All decoder tests passed!" << std::endl;
    return 0;
}
