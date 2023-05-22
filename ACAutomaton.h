#ifndef ACAUTOMATON_H
#define ACAUTOMATON_H

#include <string>

class ACAutomaton {
public:
    ACAutomaton();
    ~ACAutomaton();

    void insert(std::string word);
    void build();
    std::string filter(std::string sentence);

private:
    struct Node;
    Node* root;

    static std::u32string utf8_to_utf32(std::string str);
    static std::string utf32_to_utf8(std::u32string str);
    void release(Node* node);
};

#endif // ACAUTOMATON_H