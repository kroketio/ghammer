#include "lib/vmfpp/structs/Node.h"

#include <stdexcept>

using namespace vmfpp;

const Node::ValueMap& Node::getValues() const {
	return this->values;
}

Node::ValueMap& Node::getValues() {
	return this->values;
}

bool Node::hasValue(std::string_view key) const {
	return this->values.contains(key);
}

const std::vector<std::string>& Node::getValue(std::string_view key) const {
	const auto it = this->values.find(key);
	if (it == this->values.end()) {
		throw std::out_of_range(std::string(key));
	}
	return it->second;
}

std::vector<std::string>& Node::getValue(std::string_view key) {
	const auto it = this->values.find(key);
	if (it == this->values.end()) {
		throw std::out_of_range(std::string(key));
	}
	return it->second;
}

const std::vector<std::string>* Node::findValue(std::string_view key) const {
	const auto it = this->values.find(key);
	return it == this->values.end() ? nullptr : &it->second;
}

std::vector<std::string>* Node::findValueMut(std::string_view key) {
	const auto it = this->values.find(key);
	return it == this->values.end() ? nullptr : &it->second;
}

void Node::addValue(std::string key, std::string value) {
	this->values[std::move(key)].push_back(std::move(value));
}

void Node::removeValue(std::string_view key) {
	const auto it = this->values.find(key);
	if (it != this->values.end()) {
		this->values.erase(it);
	}
}

const Node::ChildMap& Node::getChildren() const {
	return this->children;
}

Node::ChildMap& Node::getChildren() {
	return this->children;
}

bool Node::hasChild(std::string_view key) const {
	return this->children.contains(key);
}

const std::vector<Node>& Node::getChild(std::string_view key) const {
	const auto it = this->children.find(key);
	if (it == this->children.end()) {
		throw std::out_of_range(std::string(key));
	}
	return it->second;
}

std::vector<Node>& Node::getChild(std::string_view key) {
	const auto it = this->children.find(key);
	if (it == this->children.end()) {
		throw std::out_of_range(std::string(key));
	}
	return it->second;
}

const std::vector<Node>* Node::findChild(std::string_view key) const {
	const auto it = this->children.find(key);
	return it == this->children.end() ? nullptr : &it->second;
}

std::vector<Node>* Node::findChildMut(std::string_view key) {
	const auto it = this->children.find(key);
	return it == this->children.end() ? nullptr : &it->second;
}

void Node::addChild(std::string key, Node&& value) {
	this->children[std::move(key)].push_back(std::move(value));
}

void Node::removeChild(std::string_view key) {
	const auto it = this->children.find(key);
	if (it != this->children.end()) {
		this->children.erase(it);
	}
}

std::size_t Node::getNumItems() const {
	return this->values.size() + this->children.size();
}
