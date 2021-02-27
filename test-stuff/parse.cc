#include <stdio.h>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>

std::vector<std::string> split(const std::string& s, char delimiter)
{
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) {
	if (token != "")
	    tokens.push_back(token);
    }
    return tokens;
}

int main(int argc, char **argv)
{
    auto list = split(std::string(argv[1]), '/');

    auto leaf = list.back();
    list.pop_back();

    std::cout << "leaf: " << leaf << std::endl;
    for (auto it = list.begin(); it != list.end(); it++)
	std::cout << "." << *it << "." << std::endl;
}

    
