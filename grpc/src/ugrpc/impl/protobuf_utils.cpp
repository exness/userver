#include <ugrpc/impl/protobuf_utils.hpp>

#include <unordered_set>

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/text_format.h>
#include <grpcpp/support/string_ref.h>
#include <boost/container/small_vector.hpp>

#include <userver/compiler/thread_local.hpp>
#include <userver/utils/numeric_cast.hpp>

#include <userver/ugrpc/protobuf_visit.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::impl {

namespace {

[[maybe_unused]] bool HasDebugRedactOption([[maybe_unused]] const google::protobuf::FieldDescriptor& field) {
#if defined(ARCADIA_ROOT) || GOOGLE_PROTOBUF_VERSION >= 4022000
    return field.options().debug_redact();
#else
    return false;
#endif
}

class LimitingOutputStream final : public google::protobuf::io::ZeroCopyOutputStream {
public:
    class LimitReachedException : public std::exception {};

    explicit LimitingOutputStream(google::protobuf::io::ArrayOutputStream& output_stream)
        : output_stream_{output_stream} {}

    /*
      Throw `LimitReachedException` on limit reached
    */
    bool Next(void** data, int* size) override {
        if (!output_stream_.Next(data, size)) {
            limit_reached_ = true;
            throw LimitReachedException{};
        }
        return true;
    }

    void BackUp(int count) override {
        if (!limit_reached_) {
            output_stream_.BackUp(count);
        }
    }

    int64_t ByteCount() const override { return output_stream_.ByteCount(); }

private:
    google::protobuf::io::ArrayOutputStream& output_stream_;
    bool limit_reached_{false};
};

class HideFieldValuePrinter final : public google::protobuf::TextFormat::FastFieldValuePrinter {
public:
    using BaseTextGenerator = google::protobuf::TextFormat::BaseTextGenerator;

    void PrintBool(bool, BaseTextGenerator* generator) const override { PrintRedacted(generator); }

    void PrintInt32(std::int32_t, BaseTextGenerator* generator) const override { PrintRedacted(generator); }

    void PrintUInt32(std::uint32_t, BaseTextGenerator* generator) const override { PrintRedacted(generator); }

#if defined(ARCADIA_ROOT)
    void PrintInt64(long, BaseTextGenerator* generator) const override { PrintRedacted(generator); }

    void PrintUInt64(unsigned long, BaseTextGenerator* generator) const override { PrintRedacted(generator); }
#else
    void PrintInt64(std::int64_t, BaseTextGenerator* generator) const override { PrintRedacted(generator); }

    void PrintUInt64(std::uint64_t, BaseTextGenerator* generator) const override { PrintRedacted(generator); }
#endif

    void PrintFloat(float, BaseTextGenerator* generator) const override { PrintRedacted(generator); }

    void PrintDouble(double, BaseTextGenerator* generator) const override { PrintRedacted(generator); }

    void PrintString(const grpc::string&, BaseTextGenerator* generator) const override { PrintRedacted(generator); }

    void PrintBytes(const grpc::string&, BaseTextGenerator* generator) const override { PrintRedacted(generator); }

    void PrintEnum(std::int32_t, const grpc::string&, BaseTextGenerator* generator) const override {
        PrintRedacted(generator);
    }

    bool PrintMessageContent(
        const google::protobuf::Message& /*message*/,
        int /*fieldIndex*/,
        int /*fieldCount*/,
        bool singleLineMode,
        BaseTextGenerator* generator
    ) const override {
        PrintRedacted(generator);
        if (singleLineMode) {
            generator->PrintLiteral(" ");
        } else {
            generator->PrintLiteral("\n");
        }
        // don't use default printing logic
        return true;
    }

    void PrintMessageEnd(
        const google::protobuf::Message& /*message*/,
        int /*field_index*/,
        int /*field_count*/,
        bool /*single_line_mode*/,
        BaseTextGenerator* /*generator*/
    ) const override {
        // noop
    }

    void PrintMessageStart(
        const google::protobuf::Message& /*message*/,
        int /*field_index*/,
        int /*field_count*/,
        bool /*single_line_mode*/,
        BaseTextGenerator* generator
    ) const override {
        generator->PrintLiteral(": ");
    }

private:
    static void PrintRedacted(BaseTextGenerator* generator) { generator->PrintLiteral("[REDACTED]"); }
};

class SecretFieldsPrinter final {
public:
    SecretFieldsPrinter() {
        printer_.SetUseUtf8StringEscaping(true);
        printer_.SetExpandAny(true);
    }

    void VisitAllDescriptors(const google::protobuf::Descriptor* desc) {
        if (!desc) {
            return;
        }
        const auto [_, inserted] = registered_.insert(desc);
        if (inserted) {
            for (int i = 0; i < desc->field_count(); ++i) {
                const google::protobuf::FieldDescriptor* field = desc->field(i);
                UASSERT(field);
                if (HasDebugRedactOption(*field)) {
                    RegisterSecretFieldValuePrinter(*field);
                }
                VisitAllDescriptors(field->message_type());
            }
        }
    }

    void Print(const google::protobuf::Message& message, LimitingOutputStream& stream) {
        printer_.Print(message, &stream);
    }

private:
    void RegisterSecretFieldValuePrinter(const google::protobuf::FieldDescriptor& field) {
        auto printer = std::make_unique<HideFieldValuePrinter>();

        if (!printer_.RegisterFieldValuePrinter(&field, printer.get())) {
            throw std::runtime_error{
                fmt::format("Failed to register the printer for the field: '{}'", field.full_name())};
        }
        // RegisterFieldValuePrinter took an ownership of the printer => release it.
        [[maybe_unused]] const auto ptr = printer.release();
    }

    using Registered = std::unordered_set<const google::protobuf::Descriptor*>;

    Registered registered_;
    google::protobuf::TextFormat::Printer printer_;
};

compiler::ThreadLocal kSecretFieldsPrinter = [] { return SecretFieldsPrinter{}; };

}  // namespace

bool IsMessage(const google::protobuf::FieldDescriptor& field) {
    return field.type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE ||
           field.type() == google::protobuf::FieldDescriptor::TYPE_GROUP;
}

std::string ToLimitedDebugString(const google::protobuf::Message& message, std::size_t limit) {
    auto printer = kSecretFieldsPrinter.Use();
    printer->VisitAllDescriptors(message.GetDescriptor());

    boost::container::small_vector<char, 1024> output_buffer{limit, boost::container::default_init};
    google::protobuf::io::ArrayOutputStream output_stream{output_buffer.data(), utils::numeric_cast<int>(limit)};
    LimitingOutputStream limiting_output_stream{output_stream};

    // Throwing an exception is apparently the only way to terminate message traversal once size limit is reached.
    try {
        printer->Print(message, limiting_output_stream);
    } catch (const LimitingOutputStream::LimitReachedException& /*ex*/) {
        // Buffer limit has been reached.
    }

    return std::string{output_buffer.data(), static_cast<std::size_t>(output_stream.ByteCount())};
}

}  // namespace ugrpc::impl

USERVER_NAMESPACE_END
