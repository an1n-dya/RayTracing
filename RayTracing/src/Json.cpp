#include "Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace Json {

	static const Value s_Null;

	const Value& Value::operator[](size_t index) const {
		return IsArray() && index < m_Array.size() ? m_Array[index] : s_Null;
	}

	Value& Value::Append(Value value) {
		if (IsNull())
			m_Type = Type::Array;
		m_Array.push_back(std::move(value));
		return m_Array.back();
	}

	const Value* Value::Find(const std::string& key) const {
		if (!IsObject())
			return nullptr;
		for (const auto& [name, value] : m_Object)
			if (name == key)
				return &value;
		return nullptr;
	}

	const Value& Value::operator[](const std::string& key) const {
		const Value* value = Find(key);
		return value ? *value : s_Null;
	}

	Value& Value::operator[](const std::string& key) {
		if (IsNull())
			m_Type = Type::Object;
		for (auto& [name, value] : m_Object)
			if (name == key)
				return value;
		m_Object.emplace_back(key, Value());
		return m_Object.back().second;
	}

	// ---- Parsing ----

	namespace {

		class Parser {
		public:
			Parser(const std::string& text) : m_Text(text) {}

			bool ParseDocument(Value& out) {
				SkipWhitespace();
				if (!ParseValue(out, 0))
					return false;
				SkipWhitespace();
				if (m_Pos != m_Text.size())
					return Fail("unexpected trailing characters");
				return true;
			}

			const std::string& GetError() const { return m_Error; }

		private:
			bool Fail(const std::string& message) {
				if (!m_Error.empty())
					return false;
				int line = 1, column = 1;
				for (size_t i = 0; i < m_Pos && i < m_Text.size(); i++) {
					if (m_Text[i] == '\n') { line++; column = 1; }
					else column++;
				}
				m_Error = std::to_string(line) + ":" + std::to_string(column) + ": " + message;
				return false;
			}

			void SkipWhitespace() {
				while (m_Pos < m_Text.size() && (m_Text[m_Pos] == ' ' || m_Text[m_Pos] == '\t' || m_Text[m_Pos] == '\n' || m_Text[m_Pos] == '\r'))
					m_Pos++;
			}

			bool Match(const char* literal) {
				size_t length = strlen(literal);
				if (m_Text.compare(m_Pos, length, literal) != 0)
					return false;
				m_Pos += length;
				return true;
			}

			bool ParseValue(Value& out, int depth) {
				if (depth > 256)
					return Fail("nesting too deep");
				if (m_Pos >= m_Text.size())
					return Fail("unexpected end of input");

				char c = m_Text[m_Pos];
				if (c == '{') return ParseObject(out, depth);
				if (c == '[') return ParseArray(out, depth);
				if (c == '"') {
					std::string text;
					if (!ParseString(text))
						return false;
					out = Value(std::move(text));
					return true;
				}
				if (Match("true")) { out = Value(true); return true; }
				if (Match("false")) { out = Value(false); return true; }
				if (Match("null")) { out = Value(); return true; }
				if (c == '-' || (c >= '0' && c <= '9'))
					return ParseNumber(out);
				return Fail(std::string("unexpected character '") + c + "'");
			}

			bool ParseNumber(Value& out) {
				const char* begin = m_Text.c_str() + m_Pos;
				char* end = nullptr;
				double number = strtod(begin, &end);
				if (end == begin)
					return Fail("invalid number");
				m_Pos += (size_t)(end - begin);
				out = Value(number);
				return true;
			}

			static void AppendUTF8(std::string& text, uint32_t codepoint) {
				if (codepoint < 0x80) {
					text += (char)codepoint;
				}
				else if (codepoint < 0x800) {
					text += (char)(0xC0 | (codepoint >> 6));
					text += (char)(0x80 | (codepoint & 0x3F));
				}
				else if (codepoint < 0x10000) {
					text += (char)(0xE0 | (codepoint >> 12));
					text += (char)(0x80 | ((codepoint >> 6) & 0x3F));
					text += (char)(0x80 | (codepoint & 0x3F));
				}
				else {
					text += (char)(0xF0 | (codepoint >> 18));
					text += (char)(0x80 | ((codepoint >> 12) & 0x3F));
					text += (char)(0x80 | ((codepoint >> 6) & 0x3F));
					text += (char)(0x80 | (codepoint & 0x3F));
				}
			}

			bool ParseHex4(uint32_t& value) {
				if (m_Pos + 4 > m_Text.size())
					return Fail("truncated \\u escape");
				value = 0;
				for (int i = 0; i < 4; i++) {
					char c = m_Text[m_Pos++];
					value <<= 4;
					if (c >= '0' && c <= '9') value |= (uint32_t)(c - '0');
					else if (c >= 'a' && c <= 'f') value |= (uint32_t)(c - 'a' + 10);
					else if (c >= 'A' && c <= 'F') value |= (uint32_t)(c - 'A' + 10);
					else return Fail("invalid \\u escape");
				}
				return true;
			}

			bool ParseString(std::string& out) {
				m_Pos++; // opening quote
				while (true) {
					if (m_Pos >= m_Text.size())
						return Fail("unterminated string");
					char c = m_Text[m_Pos++];
					if (c == '"')
						return true;
					if (c != '\\') {
						out += c;
						continue;
					}
					if (m_Pos >= m_Text.size())
						return Fail("unterminated string");
					char escape = m_Text[m_Pos++];
					switch (escape) {
					case '"': out += '"'; break;
					case '\\': out += '\\'; break;
					case '/': out += '/'; break;
					case 'b': out += '\b'; break;
					case 'f': out += '\f'; break;
					case 'n': out += '\n'; break;
					case 'r': out += '\r'; break;
					case 't': out += '\t'; break;
					case 'u': {
						uint32_t codepoint;
						if (!ParseHex4(codepoint))
							return false;
						// Surrogate pair
						if (codepoint >= 0xD800 && codepoint <= 0xDBFF && Match("\\u")) {
							uint32_t low;
							if (!ParseHex4(low))
								return false;
							codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
						}
						AppendUTF8(out, codepoint);
						break;
					}
					default:
						return Fail(std::string("invalid escape '\\") + escape + "'");
					}
				}
			}

			bool ParseArray(Value& out, int depth) {
				m_Pos++; // [
				out = Value::MakeArray();
				SkipWhitespace();
				if (m_Pos < m_Text.size() && m_Text[m_Pos] == ']') {
					m_Pos++;
					return true;
				}
				while (true) {
					SkipWhitespace();
					Value element;
					if (!ParseValue(element, depth + 1))
						return false;
					out.Append(std::move(element));
					SkipWhitespace();
					if (m_Pos >= m_Text.size())
						return Fail("unterminated array");
					char c = m_Text[m_Pos++];
					if (c == ']')
						return true;
					if (c != ',')
						return Fail("expected ',' or ']' in array");
				}
			}

			bool ParseObject(Value& out, int depth) {
				m_Pos++; // {
				out = Value::MakeObject();
				SkipWhitespace();
				if (m_Pos < m_Text.size() && m_Text[m_Pos] == '}') {
					m_Pos++;
					return true;
				}
				while (true) {
					SkipWhitespace();
					if (m_Pos >= m_Text.size() || m_Text[m_Pos] != '"')
						return Fail("expected a string key");
					std::string key;
					if (!ParseString(key))
						return false;
					SkipWhitespace();
					if (m_Pos >= m_Text.size() || m_Text[m_Pos] != ':')
						return Fail("expected ':' after key");
					m_Pos++;
					SkipWhitespace();
					if (!ParseValue(out[key], depth + 1))
						return false;
					SkipWhitespace();
					if (m_Pos >= m_Text.size())
						return Fail("unterminated object");
					char c = m_Text[m_Pos++];
					if (c == '}')
						return true;
					if (c != ',')
						return Fail("expected ',' or '}' in object");
				}
			}

		private:
			const std::string& m_Text;
			size_t m_Pos = 0;
			std::string m_Error;
		};

	}

	bool Parse(const std::string& text, Value& out, std::string* error) {
		Parser parser(text);
		Value result;
		if (!parser.ParseDocument(result)) {
			if (error)
				*error = parser.GetError();
			return false;
		}
		out = std::move(result);
		return true;
	}

	// ---- Writing ----

	namespace {

		// Shortest representation that reads back as the same *float* - scene values are all floats, so 0.1f is
		// written as 0.1 rather than 0.100000001 (9 significant digits always round-trip a float)
		std::string FormatNumber(double number) {
			if (!std::isfinite(number))
				return "0";
			if (number == std::floor(number) && std::fabs(number) < 1e15)
				return std::to_string((long long)number);

			char buffer[32];
			for (int precision = 1; precision <= 9; precision++) {
				snprintf(buffer, sizeof(buffer), "%.*g", precision, number);
				if ((float)strtod(buffer, nullptr) == (float)number)
					break;
			}
			return buffer;
		}

		void WriteString(std::string& out, const std::string& text) {
			out += '"';
			for (char c : text) {
				switch (c) {
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				case '\b': out += "\\b"; break;
				case '\f': out += "\\f"; break;
				default:
					if ((unsigned char)c < 0x20) {
						char buffer[8];
						snprintf(buffer, sizeof(buffer), "\\u%04x", (unsigned)(unsigned char)c);
						out += buffer;
					}
					else {
						out += c;
					}
				}
			}
			out += '"';
		}

		bool IsFlat(const Value& value) {
			for (const Value& element : value.Elements())
				if (!element.IsNumber() && !element.IsBool() && !element.IsNull())
					return false;
			return true;
		}

		void WriteValue(std::string& out, const Value& value, int indent, int depth) {
			auto newline = [&](int level) {
				out += '\n';
				out.append((size_t)(indent * level), ' ');
			};

			switch (value.GetType()) {
			case Value::Type::Null:   out += "null"; break;
			case Value::Type::Bool:   out += value.AsBool() ? "true" : "false"; break;
			case Value::Type::Number: out += FormatNumber(value.AsNumber()); break;
			case Value::Type::String: WriteString(out, value.AsString()); break;
			case Value::Type::Array: {
				const auto& elements = value.Elements();
				if (elements.empty()) {
					out += "[]";
					break;
				}
				bool flat = IsFlat(value);
				out += '[';
				for (size_t i = 0; i < elements.size(); i++) {
					if (i > 0)
						out += flat ? ", " : ",";
					if (!flat)
						newline(depth + 1);
					WriteValue(out, elements[i], indent, depth + 1);
				}
				if (!flat)
					newline(depth);
				out += ']';
				break;
			}
			case Value::Type::Object: {
				const auto& members = value.Members();
				if (members.empty()) {
					out += "{}";
					break;
				}
				out += '{';
				for (size_t i = 0; i < members.size(); i++) {
					if (i > 0)
						out += ',';
					newline(depth + 1);
					WriteString(out, members[i].first);
					out += ": ";
					WriteValue(out, members[i].second, indent, depth + 1);
				}
				newline(depth);
				out += '}';
				break;
			}
			}
		}

	}

	std::string Write(const Value& value, int indent) {
		std::string out;
		WriteValue(out, value, indent, 0);
		out += '\n';
		return out;
	}

	bool ReadFile(const std::string& path, Value& out, std::string* error) {
		std::ifstream file(path, std::ios::binary);
		if (!file) {
			if (error)
				*error = "can't open " + path;
			return false;
		}
		std::stringstream contents;
		contents << file.rdbuf();
		std::string parseError;
		if (!Parse(contents.str(), out, &parseError)) {
			if (error)
				*error = path + ":" + parseError;
			return false;
		}
		return true;
	}

	bool WriteFile(const std::string& path, const Value& value, std::string* error) {
		std::ofstream file(path, std::ios::binary);
		if (!file) {
			if (error)
				*error = "can't write " + path;
			return false;
		}
		file << Write(value);
		return (bool)file;
	}

}
