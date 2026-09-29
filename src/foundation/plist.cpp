// Property lists: XML and binary reading, XML writing.
#include "foundation/foundation.h"
#include <filesystem>
#include "libc/format.h"
#include <fstream>

namespace ns {

namespace {

std::string xml_unescape(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        size_t semi = s.find(';', i);
        if (semi == std::string_view::npos) {
            out += s[i];
            continue;
        }
        std::string_view ent = s.substr(i + 1, semi - i - 1);
        if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "amp") out += '&';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            u32 cp = ent.size() > 1 && ent[1] == 'x' ? std::strtoul(std::string(ent.substr(2)).c_str(), nullptr, 16)
                                                     : std::strtoul(std::string(ent.substr(1)).c_str(), nullptr, 10);
            libc::append_utf8(out, cp);
        }
        i = semi;
    }
    return out;
}

std::string xml_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '&': out += "&amp;"; break;
        default: out += c;
        }
    }
    return out;
}

std::vector<u8> base64_decode(std::string_view s) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<u8> out;
    u32 buf = 0;
    int bits = 0;
    for (char c : s) {
        int v = val(c);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((u8)(buf >> bits));
        }
    }
    return out;
}

std::string base64_encode(const std::vector<u8>& b) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < b.size(); i += 3) {
        u32 v = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
        out += t[v >> 18], out += t[(v >> 12) & 63], out += t[(v >> 6) & 63], out += t[v & 63];
    }
    if (i + 1 == b.size()) {
        u32 v = b[i] << 16;
        out += t[v >> 18], out += t[(v >> 12) & 63], out += "==";
    } else if (i + 2 == b.size()) {
        u32 v = (b[i] << 16) | (b[i + 1] << 8);
        out += t[v >> 18], out += t[(v >> 12) & 63], out += t[(v >> 6) & 63], out += '=';
    }
    return out;
}

// Minimal recursive-descent XML plist parser (returns retained objects).
struct XmlParser {
    std::string_view s;
    size_t p = 0;

    void skip_misc() {
        for (;;) {
            while (p < s.size() && isspace((u8)s[p])) p++;
            if (s.substr(p, 4) == "<!--") p = s.find("-->", p) + 3;
            else if (s.substr(p, 2) == "<?" || s.substr(p, 2) == "<!") p = s.find('>', p) + 1;
            else break;
        }
    }
    // Reads "<tag ...>" or "<tag/>"; returns tag name, sets empty for self-closing.
    std::string open_tag(bool& empty) {
        skip_misc();
        if (p >= s.size() || s[p] != '<') return {};
        size_t end = s.find('>', p);
        std::string_view inner = s.substr(p + 1, end - p - 1);
        p = end + 1;
        empty = !inner.empty() && inner.back() == '/';
        if (empty) inner.remove_suffix(1);
        size_t sp = inner.find_first_of(" \t\r\n");
        return std::string(inner.substr(0, sp));
    }
    std::string text_until_close(const std::string& tag) {
        size_t end = s.find("</" + tag, p);
        std::string t = xml_unescape(s.substr(p, end - p));
        p = s.find('>', end) + 1;
        return t;
    }
    id parse_value() {
        bool empty = false;
        std::string tag = open_tag(empty);
        if (tag.empty() || tag[0] == '/') return 0;
        if (tag == "plist") {
            id v = parse_value();
            skip_misc();
            return v;
        }
        if (tag == "dict") {
            id d = objc::retain(mutable_dict());
            if (empty) return d;
            for (;;) {
                bool e2;
                size_t save = p;
                std::string t = open_tag(e2);
                if (t == "/dict" || t.empty()) break;
                if (t != "key") {
                    p = save;
                    break;
                }
                std::string key = e2 ? "" : text_until_close("key");
                id v = parse_value();
                if (v) {
                    dict_set(d, str(key), v);
                    objc::release(v);
                }
            }
            return d;
        }
        if (tag == "array") {
            id a = objc::retain(mutable_array());
            if (empty) return a;
            for (;;) {
                skip_misc();
                if (s.substr(p, 8) == "</array>") {
                    p += 8;
                    break;
                }
                id v = parse_value();
                if (!v) break;
                objc::send(a, "addObject:", {v});
                objc::release(v);
            }
            return a;
        }
        if (tag == "string") return str_retained(empty ? "" : text_until_close("string"));
        if (tag == "integer") return objc::retain(number_int(std::strtoll(text_until_close("integer").c_str(), nullptr, 10)));
        if (tag == "real") return objc::retain(number_double(std::strtod(text_until_close("real").c_str(), nullptr)));
        if (tag == "true") return objc::retain(number_bool(true));
        if (tag == "false") return objc::retain(number_bool(false));
        if (tag == "date") return str_retained(text_until_close("date"));
        if (tag == "data") {
            auto b = base64_decode(empty ? "" : text_until_close("data"));
            return objc::retain(data_with(b.data(), b.size()));
        }
        LOG_WARN("plist: unknown tag <%s>", tag.c_str());
        return 0;
    }
};

// Binary plist ("bplist00").
struct BinParser {
    const std::vector<u8>& b;
    int offset_size = 0, ref_size = 0;
    u64 num_objects = 0, top = 0, table = 0;

    u64 be(u64 off, int n) const {
        u64 v = 0;
        for (int i = 0; i < n; i++) v = (v << 8) | b[off + i];
        return v;
    }
    u64 obj_offset(u64 ref) const { return be(table + ref * offset_size, offset_size); }
    std::pair<u64, u64> len_and_start(u64 off) const {
        u8 low = b[off] & 0xf;
        if (low != 0xf) return {low, off + 1};
        u8 marker = b[off + 1];
        int n = 1 << (marker & 0xf);
        return {be(off + 2, n), off + 2 + n};
    }
    id parse(u64 ref, int depth = 0) {
        if (depth > 64) return 0;
        u64 off = obj_offset(ref);
        u8 m = b[off];
        switch (m >> 4) {
        case 0x0:
            if (m == 0x08) return objc::retain(number_bool(false));
            if (m == 0x09) return objc::retain(number_bool(true));
            return objc::retain(null_object());
        case 0x1: return objc::retain(number_int((s64)be(off + 1, 1 << (m & 0xf))));
        case 0x2: {
            u64 raw = be(off + 1, 1 << (m & 0xf));
            double d;
            if ((m & 0xf) == 2) {
                float f;
                u32 r32 = (u32)raw;
                std::memcpy(&f, &r32, 4);
                d = f;
            } else {
                std::memcpy(&d, &raw, 8);
            }
            return objc::retain(number_double(d));
        }
        case 0x3: return objc::retain(number_double(0));  // date
        case 0x4: {
            auto [n, st] = len_and_start(off);
            return objc::retain(data_with(&b[st], n));
        }
        case 0x5: {
            auto [n, st] = len_and_start(off);
            return str_retained(std::string(reinterpret_cast<const char*>(&b[st]), n));
        }
        case 0x6: {
            auto [n, st] = len_and_start(off);
            std::u16string s;
            for (u64 i = 0; i < n; i++) s += (char16_t)be(st + 2 * i, 2);
            return str16_retained(s);
        }
        case 0xa: {
            auto [n, st] = len_and_start(off);
            id a = objc::retain(mutable_array());
            for (u64 i = 0; i < n; i++) {
                id v = parse(be(st + i * ref_size, ref_size), depth + 1);
                if (v) objc::send(a, "addObject:", {v}), objc::release(v);
            }
            return a;
        }
        case 0xd: {
            auto [n, st] = len_and_start(off);
            id d = objc::retain(mutable_dict());
            for (u64 i = 0; i < n; i++) {
                id k = parse(be(st + i * ref_size, ref_size), depth + 1);
                id v = parse(be(st + (n + i) * ref_size, ref_size), depth + 1);
                if (k && v) dict_set(d, k, v);
                objc::release(k);
                objc::release(v);
            }
            return d;
        }
        default:
            LOG_WARN("bplist: unsupported object type 0x%02x", m);
            return 0;
        }
    }
};

void write_xml(std::string& out, id o, int indent) {
    std::string pad(indent, '\t');
    if (!o) return;
    if (is_string(o)) {
        out += pad + "<string>" + xml_escape(utf8(o)) + "</string>\n";
    } else if (is_number(o)) {
        const char* t = gptr<char>(objc::send(o, "objCType"));
        if (t[0] == 'c' || t[0] == 'B') out += pad + (number_int_value(o) ? "<true/>\n" : "<false/>\n");
        else if (t[0] == 'd' || t[0] == 'f') out += pad + "<real>" + utf8(objc::send(o, "description")) + "</real>\n";
        else out += pad + "<integer>" + std::to_string(number_int_value(o)) + "</integer>\n";
    } else if (is_data(o)) {
        out += pad + "<data>" + base64_encode(data_bytes(o)) + "</data>\n";
    } else if (is_dict(o)) {
        out += pad + "<dict>\n";
        for (auto& [k, v] : dict_items(o)) {
            out += pad + "\t<key>" + xml_escape(utf8(k)) + "</key>\n";
            write_xml(out, v, indent + 1);
        }
        out += pad + "</dict>\n";
    } else if (is_array(o)) {
        out += pad + "<array>\n";
        for (id v : array_items(o)) write_xml(out, v, indent + 1);
        out += pad + "</array>\n";
    } else {
        out += pad + "<string>" + xml_escape(objc::describe(o)) + "</string>\n";
    }
}

}  // namespace

id plist_from_xml(const std::string& xml) {
    XmlParser p{xml};
    return p.parse_value();
}

id plist_from_file(const std::string& host_path) {
    if (host_path.empty()) return 0;
    std::ifstream f(std::filesystem::path(libc::utf8_to_wide(host_path)), std::ios::binary);
    if (!f) return 0;
    std::vector<u8> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() >= 40 && std::memcmp(b.data(), "bplist00", 8) == 0) {
        BinParser bp{b};
        size_t t = b.size() - 32;
        bp.offset_size = b[t + 6];
        bp.ref_size = b[t + 7];
        bp.num_objects = bp.be(t + 8, 8);
        bp.top = bp.be(t + 16, 8);
        bp.table = bp.be(t + 24, 8);
        return bp.parse(bp.top);
    }
    return plist_from_xml(std::string(b.begin(), b.end()));
}

std::string plist_to_xml(id obj) {
    std::string out =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n";
    write_xml(out, obj, 0);
    return out + "</plist>\n";
}

}  // namespace ns
