#include "ACAutomaton.h"
#include <queue>
#include <unordered_map>
#include <vector>
#include <locale>
#include <codecvt>

// 定义自动机节点
struct ACAutomaton::Node {
    std::unordered_map<char32_t, Node*> next; // 用char32_t作为key，可以支持中文字符
    bool isEnd;  // 表示当前节点是否为一个关键词的结束
    int length;  // 保存关键词的长度
    Node* fail;  // 失败指针，表示当前字符匹配失败后应该转向的节点
    Node() : isEnd(false), length(0), fail(nullptr) {}
};

ACAutomaton::ACAutomaton() : root(new Node()) {}

ACAutomaton::~ACAutomaton() {
    release(root);
}

// 插入关键词，构建Trie树
void ACAutomaton::insert(std::string word) {
    if (word.empty()) return;
    std::u32string utf32_word = utf8_to_utf32(word);
    Node* node = root;
    for (int i = 0; i < utf32_word.size(); i++) {
        char32_t ch = utf32_word[i];
        if (node->next.count(ch) == 0) {
            node->next[ch] = new Node();
        }
        node = node->next[ch];
    }
    node->isEnd = true;  // 设置关键词结束标记
    node->length = utf32_word.size();  // 保存关键词的长度
}

// 构建失败指针，当所有关键词都插入完成后，即Trie树的结构不会再变化时调用
void ACAutomaton::build() {
    std::queue<Node*> q;
    root->fail = nullptr;
    q.push(root);
    while (!q.empty()) {
        Node* node = q.front(); q.pop();
        for (auto& it : node->next) {
            char32_t ch = it.first;
            Node* p = node->fail;
            if (node == root) {
                it.second->fail = root;
            } else {
                while (p) {
                    if (p->next.count(ch) > 0) {
                        it.second->fail = p->next[ch];
                        break;
                    }
                    p = p->fail;
                }
                if (!p) it.second->fail = root;
            }
            q.push(it.second);
        }
    }
}

// 过滤字符串，将关键词替换为'*'
std::string ACAutomaton::filter(std::string sentence) {
    std::u32string utf32_str = utf8_to_utf32(sentence);
    std::u32string result = utf32_str;
    Node* node = root;
    std::vector<std::pair<int, int>> matches;
    for (int i = 0; i < utf32_str.size(); i++) {
        char32_t ch = utf32_str[i];
        // 尝试向下一个字符转移，如果不能则跳转到fail指向的节点
        while (node->next.count(ch) == 0 && node != root) node = node->fail;
        node = node->next.count(ch) ? node->next[ch] : root;
        Node* temp = node;
        // 如果到达一个关键词的结束，则将该敏感词的起始和结束位置保存到matches中
        while (temp != root) {
            if (temp->isEnd) {
                matches.push_back({i - temp->length + 1, i});
            }
            temp = temp->fail;
        }
    }
    // 最后，根据matches中保存的所有敏感词的位置，将对应位置的字符替换为'*'
    for (auto& match : matches) {
        for (int i = match.first; i <= match.second; i++) {
            result[i] = '*';
        }
    }
    return utf32_to_utf8(result);
}

std::u32string ACAutomaton::utf8_to_utf32(std::string str) {
    static std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> converter;
    return converter.from_bytes(str);
}

std::string ACAutomaton::utf32_to_utf8(std::u32string str) {
    static std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> converter;
    return converter.to_bytes(str);
}

// 释放内存
void ACAutomaton::release(Node* node) {
    for (auto& it : node->next) {
        release(it.second);
    }
    delete node;
}