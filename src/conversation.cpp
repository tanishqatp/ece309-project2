#include "core/conversation.h"
#include <stdexcept>
#include <string>
#include <utility>

Conversation::~Conversation() {
    delete[] data_;
}

Conversation::Conversation(const Conversation& other) {
    if (other.size_ == 0) return;

    Message* buf = new Message[other.size_];
    try {
        for (std::size_t i = 0; i < other.size_; ++i) buf[i] = other.data_[i];
    } catch (...) {
        delete[] buf;
        throw;
    }
    data_ = buf;
    size_ = capacity_ = other.size_;
}

Conversation& Conversation::operator=(const Conversation& other) {
    Conversation tmp(other);
    swap(tmp);
    return *this;
}

Conversation::Conversation(Conversation&& other) noexcept
    : data_(other.data_), size_(other.size_), capacity_(other.capacity_) {
    other.data_ = nullptr;
    other.size_ = 0;
    other.capacity_ = 0;
}

Conversation& Conversation::operator=(Conversation&& other) noexcept {
    if (this != &other) {
        delete[] data_;
        data_ = other.data_;
        size_ = other.size_;
        capacity_ = other.capacity_;
        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
    }
    return *this;
}

void Conversation::swap(Conversation& other) noexcept {
    std::swap(data_, other.data_);
    std::swap(size_, other.size_);
    std::swap(capacity_, other.capacity_);
}

void Conversation::append(Message m) {
    if (m.role() == Role::System && size_ != 0) {
        throw std::logic_error("Conversation::append: system message must be first");
    }

    if (size_ == capacity_) {
        std::size_t new_cap = (capacity_ == 0) ? 1 : capacity_ * 2;
        Message* buf = new Message[new_cap];
        for (std::size_t i = 0; i < size_; ++i) buf[i] = std::move(data_[i]);
        delete[] data_;
        data_ = buf;
        capacity_ = new_cap;
    }

    data_[size_] = std::move(m);
    ++size_;
}

const Message& Conversation::at(std::size_t i) const {
    if (i >= size_) {
        throw std::out_of_range("Conversation::at: index " + std::to_string(i) +
                                " >= size " + std::to_string(size_));
    }
    return data_[i];
}
