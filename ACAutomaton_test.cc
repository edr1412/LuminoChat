#include "ACAutomaton.h"
#include <iostream>
#include <cassert>

int main() {
    ACAutomaton ac;
    std::vector<std::string> words = {"abc", "abcd", "bc", "bcd", "c"};
    for (std::string word : words) {
        ac.insert(word);
    }
    ac.build();
    std::cout << ac.filter("A girl is abc by c bcd, and bc is abcd abd, bleeding along the road") << std::endl;

    // 测试1：英文敏感词过滤
    ACAutomaton ac1;
    ac1.insert("hello");
    ac1.insert("world");
    ac1.build();
    std::string sentence1 = "hello world, how are you?";
    std::string filtered1 = ac1.filter(sentence1);
    assert(filtered1 == "***** *****, how are you?");

    // 测试2：中文敏感词过滤
    ACAutomaton ac2;
    ac2.insert("你好");
    ac2.insert("世界");
    ac2.build();
    std::string sentence2 = "你好，世界，你好世界";
    std::string filtered2 = ac2.filter(sentence2);
    assert(filtered2 == "**，**，****");

    // 测试3：英文和中文混合的敏感词过滤
    ACAutomaton ac3;
    ac3.insert("hello");
    ac3.insert("world");
    ac3.insert("你好");
    ac3.insert("世界");
    ac3.build();
    std::string sentence3 = "hello world, 你好世界";
    std::string filtered3 = ac3.filter(sentence3);
    assert(filtered3 == "***** *****, ****");

    // 测试4：相似关键词的敏感词过滤
    ACAutomaton ac4;
    ac4.insert("abc");
    ac4.insert("abcd");
    ac4.insert("bc");
    ac4.insert("bcd");
    ac4.insert("c");
    ac4.build();
    std::string sentence4 = "abcde";
    std::string filtered4 = ac4.filter(sentence4);
    assert(filtered4 == "****e");

    // 测试5：重复关键词的敏感词过滤
    ACAutomaton ac5;
    ac5.insert("hello");
    ac5.insert("hello");
    ac5.build();
    std::string sentence5 = "hello world, hello";
    std::string filtered5 = ac5.filter(sentence5);
    assert(filtered5 == "***** world, *****");

    // 测试6：空字符串的敏感词过滤
    ACAutomaton ac6;
    ac6.insert("hello");
    ac6.build();
    std::string sentence6 = "";
    std::string filtered6 = ac6.filter(sentence6);
    assert(filtered6 == "");

    // 测试7：没有敏感词的敏感词过滤
    ACAutomaton ac7;
    ac7.build();
    std::string sentence7 = "hello world";
    std::string filtered7 = ac7.filter(sentence7);
    assert(filtered7 == "hello world");

    // 测试8：敏感词包含空格的敏感词过滤
    ACAutomaton ac8;
    ac8.insert("hello world");
    ac8.build();
    std::string sentence8 = "hello world, how are you?";
    std::string filtered8 = ac8.filter(sentence8);
    assert(filtered8 == "***********, how are you?");

    // 测试9：敏感词包含标点符号的敏感词过滤
    ACAutomaton ac9;
    ac9.insert("hello, world!");
    ac9.build();
    std::string sentence9 = "hello, world! how are you?";
    std::string filtered9 = ac9.filter(sentence9);
    assert(filtered9 == "************* how are you?");

    // 测试10：敏感词包含数字的敏感词过滤
    ACAutomaton ac10;
    ac10.insert("123");
    ac10.build();
    std::string sentence10 = "123456";
    std::string filtered10 = ac10.filter(sentence10);
    assert(filtered10 == "***456");

    // 测试11：敏感词包含空字符的敏感词过滤
    ACAutomaton ac11;
    ac11.insert("hello");
    ac11.insert("");
    ac11.insert("world");
    ac11.build();
    std::string sentence11 = "hello world";
    std::string filtered11 = ac11.filter(sentence11);
    assert(filtered11 == "***** *****");

    // 测试12：敏感词包含空字符的敏感词过滤
    ACAutomaton ac12;
    ac12.insert("");
    ac12.build();
    std::string sentence12 = "hello world";
    std::string filtered12 = ac12.filter(sentence12);
    assert(filtered12 == "hello world");
    
    std::cout << "All tests passed!" << std::endl;
    return 0;
}