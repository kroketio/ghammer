#include "lib/vmfpp/vmfpp.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string_view>

using namespace vmfpp;

namespace {

constexpr std::string_view NODE_ROOT_KEY = "$root";

inline bool isBlank(char c) {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

inline bool isTokenEnd(char c) {
	return isBlank(c) || c == '{' || c == '}' || c == '\"';
}

inline void skipBlank(const char*& p, const char* end) {
	while (p != end && isBlank(*p)) {
		++p;
	}
}

inline void skipInline(const char*& p, const char* end) {
	while (p != end && (*p == ' ' || *p == '\t')) {
		++p;
	}
}

inline void skipLine(const char*& p, const char* end) {
	while (p != end && *p != '\n') {
		++p;
	}
	if (p != end) {
		++p;
	}
}

// p is on the opening quote; returns the contents and leaves p past the closing quote.
// A value whose quote is not closed on its own line keeps consuming lines until one
// carries a quote, matching the line-oriented reader this replaced.
std::string scanValue(const char*& p, const char* end) {
	++p;
	const char* start = p;
	while (p != end && *p != '\"' && *p != '\n') {
		++p;
	}
	if (p != end && *p == '\"') {
		std::string out(start, static_cast<size_t>(p - start));
		++p;
		return out;
	}

	std::string out(start, static_cast<size_t>(p - start));
	while (p != end) {
		const char* lineStart = p;
		while (p != end && *p != '\n') {
			++p;
		}
		const std::string_view line(lineStart, static_cast<size_t>(p - lineStart));
		if (p != end) {
			++p;
		}
		out += '\n';
		const size_t quote = line.find('\"');
		if (quote != std::string_view::npos) {
			out.append(line.data(), quote);
			break;
		}
		out.append(line.data(), line.size());
	}
	return out;
}

// NOLINTNEXTLINE(*-no-recursion)
bool readNode(const char*& p, const char* end, Node* scope, const std::string& key, const bool isRoot) {
	for (;;) {
		skipBlank(p, end);
		if (p == end) {
			return true;
		}

		const char c = *p;
		if (c == '}') {
			++p;
			return true;
		}
		if (c == '{') {
			++p;
			if (isRoot) {
				Node child;
				if (!readNode(p, end, &child, key, true)) {
					return false;
				}
				scope->addChild(key, std::move(child));
			}
			continue;
		}
		if (c == '\"') {
			++p;
			const char* keyStart = p;
			while (p != end && *p != '\"' && *p != '\n') {
				++p;
			}
			const std::string_view nodeKey(keyStart, static_cast<size_t>(p - keyStart));

			std::string value;
			if (p != end && *p == '\"') {
				++p;
				skipInline(p, end);
				if (p != end && *p == '\"') {
					value = scanValue(p, end);
				}
			}
			skipLine(p, end);
			scope->addValue(std::string(nodeKey), std::move(value));
			continue;
		}

		const char* nameStart = p;
		while (p != end && !isTokenEnd(*p)) {
			++p;
		}
		std::string childKey(nameStart, static_cast<size_t>(p - nameStart));
		Node child;
		if (!readNode(p, end, &child, childKey, false)) {
			return false;
		}
		scope->addChild(std::move(childKey), std::move(child));
	}
}

bool parseBuffer(const char* data, const size_t size, Node* root) {
	const char* p = data;
	const char* end = data + size;
	return readNode(p, end, root, std::string(NODE_ROOT_KEY), true);
}

} // namespace

std::optional<VMF> VMF::openMem(std::string_view data) {
	VMF vmf;
	if (!parseBuffer(data.data(), data.size(), reinterpret_cast<Node*>(&vmf))) {
		return std::nullopt;
	}
	return vmf;
}

std::optional<VMF> VMF::openFile(std::string_view filePath) {
	std::ifstream file{std::string(filePath).c_str(), std::ios_base::in | std::ios_base::binary};
	if (!file.is_open()) {
		return std::nullopt;
	}

	file.seekg(0, std::ios::end);
	const std::streamoff len = file.tellg();
	if (len <= 0) {
		return VMF{};
	}
	file.seekg(0, std::ios::beg);

	std::string buffer(static_cast<size_t>(len), '\0');
	file.read(buffer.data(), len);
	if (!file) {
		buffer.resize(static_cast<size_t>(file.gcount()));
	}
	file.close();

	VMF vmf;
	if (!parseBuffer(buffer.data(), buffer.size(), reinterpret_cast<Node*>(&vmf))) {
		return std::nullopt;
	}
	return vmf;
}

bool VMF::save(std::filesystem::path path) {
  std::ofstream f(path);
  if (!f.is_open()) {
    qWarning() << "Failed to open file: " << QString::fromStdU16String(path.u16string());
    return false;
  }

  this->_save(reinterpret_cast<vmfpp::Node*>(this), f);
  return true;
}

void VMF::_save(Node* node, std::ofstream& file, int indentLevel) {
  auto addIndent = [&file, indentLevel] {
    for (int i = 0; i < indentLevel; ++i)
      file << "\t";
  };

  if (auto values = node->getValues(); !values.empty()) {
    for (auto &[fst, snd] : values) {
      for (auto& value : snd) {
        addIndent();
        file << "\"" + fst + "\" \"" + value + "\"\n";
      }
    }
  }

  if (auto children = node->getChildren(); !children.empty()) {
    for (auto &[fst, snd] : children) {
      for (auto& _child : snd) {
        addIndent();
        file << fst + "\n";
        addIndent();
        file << "{\n";

        _save(&_child, file, indentLevel + 1);

        addIndent();
        file << "}\n";
      }
    }
  }
}

const Node::ChildMap& VMF::getSections() const {
	return this->children;
}

Node::ChildMap& VMF::getSections() {
	return this->children;
}

bool VMF::hasSection(std::string_view key) const {
	return this->hasChild(key.data());
}

const std::vector<Node>& VMF::getSection(std::string_view key) const {
	return this->getChild(key.data());
}

std::vector<Node>& VMF::getSection(std::string_view key) {
	return this->getChild(key.data());
}

void VMF::addSection(std::string key, Node value) {
	this->addChild(std::move(key), std::move(value));
}

void VMF::addSection(std::string_view key, Node value) {
	this->addChild(std::string(key), std::move(value));
}

void VMF::removeSection(std::string_view key) {
	this->removeChild(std::string(key));
}
