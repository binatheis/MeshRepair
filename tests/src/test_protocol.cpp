// Frame protocol tests: roundtrip, desync, oversize, JSON validation.
#include "engine/protocol.h"

#include <gtest/gtest.h>
#include <sstream>
#include <stdexcept>

namespace {

using namespace MeshRepair::Engine;
using nlohmann::json;

}  // namespace

TEST(Protocol, WriteReadRoundtripAllTypes)
{
    for (auto type : {MessageType::COMMAND, MessageType::RESPONSE, MessageType::EVENT}) {
        json msg;
        msg["command"] = "load_mesh";
        msg["nested"]  = {{"a", 1}, {"b", "text"}};
        msg["arr"]     = {1, 2, 3};

        std::stringstream ss;
        write_message(ss, msg, type);

        MessageType read_type;
        json back = read_message(ss, &read_type);
        EXPECT_EQ(read_type, type);
        EXPECT_EQ(back, msg);
    }
}

TEST(Protocol, WriteReadDefaultIsResponse)
{
    std::stringstream ss;
    write_message(ss, json{{"ok", true}});
    MessageType type;
    EXPECT_NO_THROW(read_message(ss, &type));
    EXPECT_EQ(type, MessageType::RESPONSE);
}

TEST(Protocol, MagicDesyncRejected)
{
    std::stringstream ss;
    // Wrong magic (0xDCBA instead of 0xABCD little-endian "BA DC")
    const char bad[] = {static_cast<char>(0xBA), static_cast<char>(0xDC), 0x00,
                        0x00, 0x00, 0x00, 0x01};
    ss.write(bad, sizeof(bad));
    EXPECT_THROW(read_message(ss), std::runtime_error);
}

TEST(Protocol, ZeroLengthRejected)
{
    std::stringstream ss;
    const char frame[] = {static_cast<char>(0xCD), static_cast<char>(0xAB),  // magic LE
                          0x00, 0x00, 0x00, 0x00,                           // length 0
                          0x01};                                             // COMMAND
    ss.write(frame, sizeof(frame));
    EXPECT_THROW(read_message(ss), std::runtime_error);
}

TEST(Protocol, OversizeRejectedBeforePayloadRead)
{
    std::stringstream ss;
    const uint32_t huge = MAX_MESSAGE_SIZE + 1;
    const char frame[] = {static_cast<char>(0xCD), static_cast<char>(0xAB),
                          static_cast<char>(huge & 0xFF), static_cast<char>((huge >> 8) & 0xFF),
                          static_cast<char>((huge >> 16) & 0xFF), static_cast<char>((huge >> 24) & 0xFF),
                          0x01};
    ss.write(frame, sizeof(frame));
    // Must fail immediately without trying to read the (absent) payload.
    EXPECT_THROW(read_message(ss), std::runtime_error);
    // And nothing was consumed from the payload because there is none.
}

TEST(Protocol, InvalidTypeRejected)
{
    std::stringstream ss;
    const uint32_t len = 2;  // "{}"
    const char frame[] = {static_cast<char>(0xCD), static_cast<char>(0xAB),
                          static_cast<char>(len & 0xFF), 0x00, 0x00, 0x00,
                          0x09};  // invalid type
    ss.write(frame, sizeof(frame));
    ss.write("{}", 2);
    EXPECT_THROW(read_message(ss), std::runtime_error);
}

TEST(Protocol, InvalidJsonRejected)
{
    std::stringstream ss;
    const uint32_t len = 5;
    const char frame[] = {static_cast<char>(0xCD), static_cast<char>(0xAB),
                          static_cast<char>(len & 0xFF), 0x00, 0x00, 0x00, 0x02};
    ss.write(frame, sizeof(frame));
    ss.write("{bad!", 5);
    EXPECT_THROW(read_message(ss), std::runtime_error);
}

TEST(Protocol, HelpersProduceExpectedShapes)
{
    json ok = create_success_response("done");
    EXPECT_EQ(ok["type"], "success");
    EXPECT_EQ(ok["message"], "done");

    json err = create_error_response("boom", "runtime");
    EXPECT_EQ(err["type"], "error");
    EXPECT_EQ(err["error"]["message"], "boom");
    EXPECT_EQ(err["error"]["type"], "runtime");

    json prog = create_progress_event(0.5, "filling");
    EXPECT_EQ(prog["type"], "progress");
    EXPECT_DOUBLE_EQ(prog["progress"].get<double>(), 0.5);
    EXPECT_EQ(prog["status"], "filling");

    json log = create_log_event("info", "hello");
    EXPECT_EQ(log["level"], "info");
    EXPECT_EQ(log["message"], "hello");

    std::string err_msg;
    json cmd;
    cmd["command"] = "init";
    EXPECT_TRUE(validate_command(cmd, "init", &err_msg));
    EXPECT_FALSE(validate_command(cmd, "other", &err_msg));
    EXPECT_FALSE(err_msg.empty());
}

TEST(Protocol, ConsecutiveMessagesOnSameStream)
{
    std::stringstream ss;
    write_message(ss, json{{"seq", 1}}, MessageType::COMMAND);
    write_message(ss, json{{"seq", 2}}, MessageType::RESPONSE);
    write_message(ss, json{{"seq", 3}}, MessageType::EVENT);

    for (int expected = 1; expected <= 3; ++expected) {
        json msg = read_message(ss);
        EXPECT_EQ(msg["seq"].get<int>(), expected);
    }
}
