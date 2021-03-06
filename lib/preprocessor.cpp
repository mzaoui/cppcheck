/*
 * Cppcheck - A tool for static C/C++ code analysis
 * Copyright (C) 2007-2012 Daniel Marjamäki and Cppcheck team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "preprocessor.h"
#include "tokenize.h"
#include "token.h"
#include "path.h"
#include "errorlogger.h"
#include "settings.h"

#include <algorithm>
#include <sstream>
#include <fstream>
#include <cstdlib>
#include <cctype>
#include <vector>
#include <set>
#include <stack>

bool Preprocessor::missingIncludeFlag;

Preprocessor::Preprocessor(Settings *settings, ErrorLogger *errorLogger) : _settings(settings), _errorLogger(errorLogger)
{

}

void Preprocessor::writeError(const std::string &fileName, const unsigned int linenr, ErrorLogger *errorLogger, const std::string &errorType, const std::string &errorText)
{
    if (!errorLogger)
        return;

    std::list<ErrorLogger::ErrorMessage::FileLocation> locationList;
    ErrorLogger::ErrorMessage::FileLocation loc;
    loc.line = linenr;
    loc.setfile(fileName);
    locationList.push_back(loc);
    errorLogger->reportErr(ErrorLogger::ErrorMessage(locationList,
                           Severity::error,
                           errorText,
                           errorType,
                           false));
}

static unsigned char readChar(std::istream &istr)
{
    unsigned char ch = (unsigned char)istr.get();

    // Handling of newlines..
    if (ch == '\r') {
        ch = '\n';
        if ((char)istr.peek() == '\n')
            (void)istr.get();
    }

    return ch;
}

// Concatenates a list of strings, inserting a separator between parts
static std::string join(const std::set<std::string>& list, char separator)
{
    std::string s;
    for (std::set<std::string>::const_iterator it = list.begin(); it != list.end(); ++it) {
        if (!s.empty())
            s += separator;

        s += *it;
    }
    return s;
}

// Removes duplicate string portions separated by the specified separator
static std::string unify(const std::string &s, char separator)
{
    std::set<std::string> parts;

    std::string::size_type prevPos = 0;
    for (std::string::size_type pos = 0; pos < s.length(); ++pos) {
        if (s[pos] == separator) {
            if (pos > prevPos)
                parts.insert(s.substr(prevPos, pos - prevPos));
            prevPos = pos + 1;
        }
    }
    if (prevPos < s.length())
        parts.insert(s.substr(prevPos));

    return join(parts, separator);
}

/** Just read the code into a string. Perform simple cleanup of the code */
std::string Preprocessor::read(std::istream &istr, const std::string &filename)
{
    // ------------------------------------------------------------------------------------------
    //
    // handling <backslash><newline>
    // when this is encountered the <backslash><newline> will be "skipped".
    // on the next <newline>, extra newlines will be added
    std::ostringstream code;
    unsigned int newlines = 0;
    for (unsigned char ch = readChar(istr); istr.good(); ch = readChar(istr)) {
        // Replace assorted special chars with spaces..
        if (((ch & 0x80) == 0) && (ch != '\n') && (std::isspace(ch) || std::iscntrl(ch)))
            ch = ' ';

        // <backslash><newline>..
        // for gcc-compatibility the trailing spaces should be ignored
        // for vs-compatibility the trailing spaces should be kept
        // See tickets #640 and #1869
        // The solution for now is to have a compiler-dependent behaviour.
        if (ch == '\\') {
            unsigned char chNext;

#ifdef __GNUC__
            // gcc-compatibility: ignore spaces
            for (;;) {
                chNext = (unsigned char)istr.peek();
                if (chNext != '\n' && chNext != '\r' &&
                    (std::isspace(chNext) || std::iscntrl(chNext))) {
                    // Skip whitespace between <backslash> and <newline>
                    (void)readChar(istr);
                    continue;
                }

                break;
            }
#else
            // keep spaces
            chNext = (unsigned char)istr.peek();
#endif
            if (chNext == '\n' || chNext == '\r') {
                ++newlines;
                (void)readChar(istr);   // Skip the "<backslash><newline>"
            } else
                code << "\\";
        } else {
            code << char(ch);

            // if there has been <backslash><newline> sequences, add extra newlines..
            if (ch == '\n' && newlines > 0) {
                code << std::string(newlines, '\n');
                newlines = 0;
            }
        }
    }
    std::string result = code.str();
    code.str("");

    // ------------------------------------------------------------------------------------------
    //
    // Remove all comments..
    result = removeComments(result, filename);

    // ------------------------------------------------------------------------------------------
    //
    // Clean up all preprocessor statements
    result = preprocessCleanupDirectives(result);

    // ------------------------------------------------------------------------------------------
    //
    // Clean up preprocessor #if statements with Parentheses
    result = removeParentheses(result);

    // Remove '#if 0' blocks
    if (result.find("#if 0\n") != std::string::npos)
        result = removeIf0(result);

    return result;
}

std::string Preprocessor::preprocessCleanupDirectives(const std::string &processedFile) const
{
    std::ostringstream code;
    std::istringstream sstr(processedFile);

    std::string line;
    while (std::getline(sstr, line)) {
        // Trim lines..
        if (!line.empty() && line[0] == ' ')
            line.erase(0, line.find_first_not_of(" "));
        if (!line.empty() && line[line.size()-1] == ' ')
            line.erase(line.find_last_not_of(" ") + 1);

        // Preprocessor
        if (!line.empty() && line[0] == '#') {
            enum {
                ESC_NONE,
                ESC_SINGLE,
                ESC_DOUBLE
            } escapeStatus = ESC_NONE;

            char prev = ' '; // hack to make it skip spaces between # and the directive
            code << "#";
            std::string::const_iterator i = line.begin();
            ++i;

            // need space.. #if( => #if (
            bool needSpace = true;
            while (i != line.end()) {
                // disable esc-mode
                if (escapeStatus != ESC_NONE) {
                    if (prev != '\\' && escapeStatus == ESC_SINGLE && *i == '\'') {
                        escapeStatus = ESC_NONE;
                    }
                    if (prev != '\\' && escapeStatus == ESC_DOUBLE && *i == '"') {
                        escapeStatus = ESC_NONE;
                    }
                } else {
                    // enable esc-mode
                    if (escapeStatus == ESC_NONE && *i == '"')
                        escapeStatus = ESC_DOUBLE;
                    if (escapeStatus == ESC_NONE && *i == '\'')
                        escapeStatus = ESC_SINGLE;
                }
                // skip double whitespace between arguments
                if (escapeStatus == ESC_NONE && prev == ' ' && *i == ' ') {
                    ++i;
                    continue;
                }
                // Convert #if( to "#if ("
                if (escapeStatus == ESC_NONE) {
                    if (needSpace) {
                        if (*i == '(' || *i == '!')
                            code << " ";
                        else if (!std::isalpha(*i))
                            needSpace = false;
                    }
                    if (*i == '#')
                        needSpace = true;
                }
                code << *i;
                if (escapeStatus != ESC_NONE && prev == '\\' && *i == '\\') {
                    prev = ' ';
                } else {
                    prev = *i;
                }
                ++i;
            }
            if (escapeStatus != ESC_NONE) {
                // unmatched quotes.. compiler should probably complain about this..
            }
        } else {
            // Do not mess with regular code..
            code << line;
        }
        code << (sstr.eof()?"":"\n");
    }

    return code.str();
}

static bool hasbom(const std::string &str)
{
    return bool(str.size() >= 3 &&
                static_cast<unsigned char>(str[0]) == 0xef &&
                static_cast<unsigned char>(str[1]) == 0xbb &&
                static_cast<unsigned char>(str[2]) == 0xbf);
}


// This wrapper exists because Sun's CC does not allow a static_cast
// from extern "C" int(*)(int) to int(*)(int).
static int tolowerWrapper(int c)
{
    return std::tolower(c);
}


static bool isFallThroughComment(std::string comment)
{
    // convert comment to lower case without whitespace
    for (std::string::iterator i = comment.begin(); i != comment.end();) {
        if (std::isspace(static_cast<unsigned char>(*i)))
            i = comment.erase(i);
        else
            ++i;
    }
    std::transform(comment.begin(), comment.end(), comment.begin(), tolowerWrapper);

    return comment.find("fallthr") != std::string::npos ||
           comment.find("fallsthr") != std::string::npos ||
           comment.find("fall-thr") != std::string::npos ||
           comment.find("dropthr") != std::string::npos ||
           comment.find("passthr") != std::string::npos ||
           comment.find("nobreak") != std::string::npos ||
           comment == "fall";
}

std::string Preprocessor::removeComments(const std::string &str, const std::string &filename)
{
    // For the error report
    unsigned int lineno = 1;

    // handling <backslash><newline>
    // when this is encountered the <backslash><newline> will be "skipped".
    // on the next <newline>, extra newlines will be added
    unsigned int newlines = 0;
    std::ostringstream code;
    unsigned char previous = 0;
    bool inPreprocessorLine = false;
    std::vector<std::string> suppressionIDs;
    bool fallThroughComment = false;

    for (std::string::size_type i = hasbom(str) ? 3U : 0U; i < str.length(); ++i) {
        unsigned char ch = static_cast<unsigned char>(str[i]);
        if (ch & 0x80) {
            std::ostringstream errmsg;
            errmsg << "The code contains characters that are unhandled. "
                   << "Neither unicode nor extended ASCII are supported. "
                   << "(line=" << lineno << ", character code=" << std::hex << (int(ch) & 0xff) << ")";
            writeError(filename, lineno, _errorLogger, "syntaxError", errmsg.str());
        }

        if ((str.compare(i, 6, "#error") == 0 && (!_settings || _settings->userDefines.empty())) ||
            str.compare(i, 8, "#warning") == 0) {
            if (str.compare(i, 6, "#error") == 0)
                code << "#error";

            i = str.find("\n", i);
            if (i == std::string::npos)
                break;

            --i;
            continue;
        }

        // First skip over any whitespace that may be present
        if (std::isspace(ch)) {
            if (ch == ' ' && previous == ' ') {
                // Skip double white space
            } else {
                code << char(ch);
                previous = ch;
            }

            // if there has been <backslash><newline> sequences, add extra newlines..
            if (ch == '\n') {
                if (previous != '\\')
                    inPreprocessorLine = false;
                ++lineno;
                if (newlines > 0) {
                    code << std::string(newlines, '\n');
                    newlines = 0;
                    previous = '\n';
                }
            }

            continue;
        }

        // Remove comments..
        if (str.compare(i, 2, "//", 0, 2) == 0) {
            size_t commentStart = i + 2;
            i = str.find('\n', i);
            if (i == std::string::npos)
                break;
            std::string comment(str, commentStart, i - commentStart);

            if (_settings && _settings->_inlineSuppressions) {
                std::istringstream iss(comment);
                std::string word;
                iss >> word;
                if (word == "cppcheck-suppress") {
                    iss >> word;
                    if (iss)
                        suppressionIDs.push_back(word);
                }
            }

            if (isFallThroughComment(comment)) {
                fallThroughComment = true;
            }

            code << "\n";
            previous = '\n';
            ++lineno;
        } else if (str.compare(i, 2, "/*", 0, 2) == 0) {
            size_t commentStart = i + 2;
            unsigned char chPrev = 0;
            ++i;
            while (i < str.length() && (chPrev != '*' || ch != '/')) {
                chPrev = ch;
                ++i;
                ch = static_cast<unsigned char>(str[i]);
                if (ch == '\n') {
                    ++newlines;
                    ++lineno;
                }
            }
            std::string comment(str, commentStart, i - commentStart - 1);

            if (isFallThroughComment(comment)) {
                fallThroughComment = true;
            }

            if (_settings && _settings->_inlineSuppressions) {
                std::istringstream iss(comment);
                std::string word;
                iss >> word;
                if (word == "cppcheck-suppress") {
                    iss >> word;
                    if (iss)
                        suppressionIDs.push_back(word);
                }
            }
        } else if (ch == '#' && previous == '\n') {
            code << ch;
            previous = ch;
            inPreprocessorLine = true;

            // Add any pending inline suppressions that have accumulated.
            if (!suppressionIDs.empty()) {
                if (_settings != NULL) {
                    // Add the suppressions.
                    for (size_t j(0); j < suppressionIDs.size(); ++j) {
                        const std::string errmsg(_settings->nomsg.addSuppression(suppressionIDs[j], filename, lineno));
                        if (!errmsg.empty()) {
                            writeError(filename, lineno, _errorLogger, "cppcheckError", errmsg);
                        }
                    }
                }
                suppressionIDs.clear();
            }
        } else {
            if (!inPreprocessorLine) {
                // Not whitespace, not a comment, and not preprocessor.
                // Must be code here!

                // First check for a "fall through" comment match, but only
                // add a suppression if the next token is 'case' or 'default'
                if (_settings && _settings->isEnabled("style") && _settings->experimental && fallThroughComment) {
                    std::string::size_type j = str.find_first_not_of("abcdefghijklmnopqrstuvwxyz", i);
                    std::string tok = str.substr(i, j - i);
                    if (tok == "case" || tok == "default")
                        suppressionIDs.push_back("switchCaseFallThrough");
                    fallThroughComment = false;
                }

                // Add any pending inline suppressions that have accumulated.
                if (!suppressionIDs.empty()) {
                    if (_settings != NULL) {
                        // Add the suppressions.
                        for (size_t j(0); j < suppressionIDs.size(); ++j) {
                            const std::string errmsg(_settings->nomsg.addSuppression(suppressionIDs[j], filename, lineno));
                            if (!errmsg.empty()) {
                                writeError(filename, lineno, _errorLogger, "cppcheckError", errmsg);
                            }
                        }
                    }
                    suppressionIDs.clear();
                }
            }

            // String or char constants..
            if (ch == '\"' || ch == '\'') {
                code << char(ch);
                char chNext;
                do {
                    ++i;
                    chNext = str[i];
                    if (chNext == '\\') {
                        ++i;
                        char chSeq = str[i];
                        if (chSeq == '\n')
                            ++newlines;
                        else {
                            code << chNext;
                            code << chSeq;
                            previous = static_cast<unsigned char>(chSeq);
                        }
                    } else {
                        code << chNext;
                        previous = static_cast<unsigned char>(chNext);
                    }
                } while (i < str.length() && chNext != ch && chNext != '\n');
            }

            // Rawstring..
            else if (str.compare(i,2,"R\"")==0) {
                std::string delim;
                for (std::string::size_type i2 = i+2; i2 < str.length(); ++i2) {
                    if (i2 > 16 ||
                        std::isspace(str[i2]) ||
                        std::iscntrl(str[i2]) ||
                        str[i2] == ')' ||
                        str[i2] == '\\') {
                        delim = " ";
                        break;
                    } else if (str[i2] == '(')
                        break;

                    delim += str[i2];
                }
                const std::string::size_type endpos = str.find(")" + delim + "\"", i);
                if (delim != " " && endpos != std::string::npos) {
                    unsigned int rawstringnewlines = 0;
                    code << '\"';
                    for (std::string::size_type p = i + 3 + delim.size(); p < endpos; ++p) {
                        if (str[p] == '\n') {
                            rawstringnewlines++;
                            code << "\\n";
                        } else if (std::iscntrl((unsigned char)str[p]) ||
                                   std::isspace((unsigned char)str[p])) {
                            code << " ";
                        } else if (str[p] == '\\') {
                            code << "\\";
                        } else if (str[p] == '\"' || str[p] == '\'') {
                            code << "\\" << (char)str[p];
                        } else {
                            code << (char)str[p];
                        }
                    }
                    code << "\"";
                    if (rawstringnewlines > 0)
                        code << std::string(rawstringnewlines, '\n');
                    i = endpos + delim.size() + 2;
                } else {
                    code << "R";
                    previous = 'R';
                }
            } else {
                code << char(ch);
                previous = ch;
            }
        }
    }

    return code.str();
}

std::string Preprocessor::removeIf0(const std::string &code)
{
    std::ostringstream ret;
    std::istringstream istr(code);
    std::string line;
    while (std::getline(istr,line)) {
        ret << line << "\n";
        if (line == "#if 0") {
            // goto the end of the '#if 0' block
            unsigned int level = 1;
            bool in = false;
            while (level > 0 && std::getline(istr,line)) {
                if (line.compare(0,3,"#if") == 0)
                    ++level;
                else if (line == "#endif")
                    --level;
                else if ((line == "#else") || (line.compare(0, 5, "#elif") == 0)) {
                    if (level == 1)
                        in = true;
                } else {
                    if (in)
                        ret << line << "\n";
                    else
                        // replace code within '#if 0' block with empty lines
                        ret << "\n";
                    continue;
                }

                ret << line << "\n";
            }
        }
    }
    return ret.str();
}


std::string Preprocessor::removeParentheses(const std::string &str)
{
    if (str.find("\n#if") == std::string::npos && str.compare(0, 3, "#if") != 0)
        return str;

    std::istringstream istr(str);
    std::ostringstream ret;
    std::string line;
    while (std::getline(istr, line)) {
        if (line.compare(0, 3, "#if") == 0 || line.compare(0, 5, "#elif") == 0) {
            std::string::size_type pos;
            pos = 0;
            while ((pos = line.find(" (", pos)) != std::string::npos)
                line.erase(pos, 1);
            pos = 0;
            while ((pos = line.find("( ", pos)) != std::string::npos)
                line.erase(pos + 1, 1);
            pos = 0;
            while ((pos = line.find(" )", pos)) != std::string::npos)
                line.erase(pos, 1);
            pos = 0;
            while ((pos = line.find(") ", pos)) != std::string::npos)
                line.erase(pos + 1, 1);

            // Remove inner parenthesis "((..))"..
            pos = 0;
            while ((pos = line.find("((", pos)) != std::string::npos) {
                ++pos;
                std::string::size_type pos2 = line.find_first_of("()", pos + 1);
                if (pos2 != std::string::npos && line[pos2] == ')') {
                    line.erase(pos2, 1);
                    line.erase(pos, 1);
                }
            }

            // "#if(A) => #if A", but avoid "#if (defined A) || defined (B)"
            if ((line.compare(0, 4, "#if(") == 0 || line.compare(0, 6, "#elif(") == 0) &&
                line[line.length() - 1] == ')') {
                int ind = 0;
                for (std::string::size_type i = 0; i < line.length(); ++i) {
                    if (line[i] == '(')
                        ++ind;
                    else if (line[i] == ')') {
                        --ind;
                        if (ind == 0) {
                            if (i == line.length() - 1) {
                                line[line.find('(')] = ' ';
                                line.erase(line.length() - 1);
                            }
                            break;
                        }
                    }
                }
            }

            if (line.compare(0, 4, "#if(") == 0)
                line.insert(3, " ");
            else if (line.compare(0, 6, "#elif(") == 0)
                line.insert(5, " ");
        }
        ret << line << "\n";
    }

    return ret.str();
}


void Preprocessor::removeAsm(std::string &str)
{
    std::string::size_type pos = 0;
    while ((pos = str.find("#asm\n", pos)) != std::string::npos) {
        str.replace(pos, 4, "asm(");

        std::string::size_type pos2 = str.find("#endasm", pos);
        if (pos2 != std::string::npos) {
            str.replace(pos2, 7, ");");
            pos = pos2;
        }
    }
}


void Preprocessor::preprocess(std::istream &istr, std::map<std::string, std::string> &result, const std::string &filename, const std::list<std::string> &includePaths)
{
    std::list<std::string> configs;
    std::string data;
    preprocess(istr, data, configs, filename, includePaths);
    for (std::list<std::string>::const_iterator it = configs.begin(); it != configs.end(); ++it) {
        if (_settings && (_settings->userUndefs.find(*it) == _settings->userUndefs.end()))
            result[ *it ] = getcode(data, *it, filename);
    }
}

std::string Preprocessor::removeSpaceNearNL(const std::string &str)
{
    std::string tmp;
    char prev = 0;
    for (unsigned int i = 0; i < str.size(); i++) {
        if (str[i] == ' ' &&
            ((i > 0 && prev == '\n') ||
             (i + 1 < str.size() && str[i+1] == '\n')
            )
           ) {
            // Ignore space that has new line in either side of it
        } else {
            tmp.append(1, str[i]);
            prev = str[i];
        }
    }

    return tmp;
}

std::string Preprocessor::replaceIfDefined(const std::string &str)
{
    std::string ret(str);
    std::string::size_type pos;

    pos = 0;
    while ((pos = ret.find("#if defined(", pos)) != std::string::npos) {
        std::string::size_type pos2 = ret.find(")", pos + 9);
        if (pos2 > ret.length() - 1)
            break;
        if (ret[pos2+1] == '\n') {
            ret.erase(pos2, 1);
            ret.erase(pos + 3, 9);
            ret.insert(pos + 3, "def ");
        }
        ++pos;
    }

    pos = 0;
    while ((pos = ret.find("#if !defined(", pos)) != std::string::npos) {
        std::string::size_type pos2 = ret.find(")", pos + 9);
        if (pos2 > ret.length() - 1)
            break;
        if (ret[pos2+1] == '\n') {
            ret.erase(pos2, 1);
            ret.erase(pos + 3, 10);
            ret.insert(pos + 3, "ndef ");
        }
        ++pos;
    }

    pos = 0;
    while ((pos = ret.find("#elif defined(", pos)) != std::string::npos) {
        std::string::size_type pos2 = ret.find(")", pos + 9);
        if (pos2 > ret.length() - 1)
            break;
        if (ret[pos2+1] == '\n') {
            ret.erase(pos2, 1);
            ret.erase(pos + 6, 8);
        }
        ++pos;
    }

    return ret;
}

void Preprocessor::preprocessWhitespaces(std::string &processedFile)
{
    // Replace all tabs with spaces..
    std::replace(processedFile.begin(), processedFile.end(), '\t', ' ');

    // Remove all indentation..
    if (!processedFile.empty() && processedFile[0] == ' ')
        processedFile.erase(0, processedFile.find_first_not_of(" "));

    // Remove space characters that are after or before new line character
    processedFile = removeSpaceNearNL(processedFile);
}

void Preprocessor::preprocess(std::istream &srcCodeStream, std::string &processedFile, std::list<std::string> &resultConfigurations, const std::string &filename, const std::list<std::string> &includePaths)
{
    if (file0.empty())
        file0 = filename;

    processedFile = read(srcCodeStream, filename);

    // Remove asm(...)
    removeAsm(processedFile);

    // Replace "defined A" with "defined(A)"
    {
        std::istringstream istr(processedFile);
        std::ostringstream ostr;
        std::string line;
        while (std::getline(istr, line)) {
            if (line.compare(0, 4, "#if ") == 0 || line.compare(0, 6, "#elif ") == 0) {
                std::string::size_type pos = 0;
                while ((pos = line.find(" defined ")) != std::string::npos) {
                    line[pos+8] = '(';
                    pos = line.find_first_of(" |&", pos + 8);
                    if (pos == std::string::npos)
                        line += ")";
                    else
                        line.insert(pos, ")");
                }
            }
            ostr << line << "\n";
        }
        processedFile = ostr.str();
    }

    if (_settings && (!_settings->userDefines.empty() || !_settings->userUndefs.empty())) {
        std::map<std::string, std::string> defs;

        // TODO: break out this code. There is other similar code.
        std::string::size_type pos1 = 0;
        while (pos1 != std::string::npos) {
            const std::string::size_type pos2 = _settings->userDefines.find_first_of(";=", pos1);
            const std::string::size_type pos3 = _settings->userDefines.find(";", pos1);

            std::string name, value;
            if (pos2 == std::string::npos)
                name = _settings->userDefines.substr(pos1);
            else
                name = _settings->userDefines.substr(pos1, pos2 - pos1);
            if (pos2 != pos3) {
                if (pos3 == std::string::npos)
                    value = _settings->userDefines.substr(pos2+1);
                else
                    value = _settings->userDefines.substr(pos2+1, pos3 - pos2 - 1);
            }

            defs[name] = value;

            pos1 = pos3;
            if (pos1 != std::string::npos)
                pos1++;
        }

        processedFile = handleIncludes(processedFile, filename, includePaths, defs);
        if (_settings->userDefines.empty())
            resultConfigurations = getcfgs(processedFile, filename);

    } else {

        handleIncludes(processedFile, filename, includePaths);

        processedFile = replaceIfDefined(processedFile);

        // Get all possible configurations..
        if (!_settings || (_settings && _settings->userDefines.empty()))
            resultConfigurations = getcfgs(processedFile, filename);
    }
}



// Get the DEF in this line: "#ifdef DEF"
std::string Preprocessor::getdef(std::string line, bool def)
{
    if (line.empty() || line[0] != '#')
        return "";

    // If def is true, the line must start with "#ifdef"
    if (def && line.compare(0, 7, "#ifdef ") != 0 && line.compare(0, 4, "#if ") != 0
        && (line.compare(0, 6, "#elif ") != 0 || line.compare(0, 7, "#elif !") == 0)) {
        return "";
    }

    // If def is false, the line must start with "#ifndef"
    if (!def && line.compare(0, 8, "#ifndef ") != 0 && line.compare(0, 7, "#elif !") != 0) {
        return "";
    }

    // Remove the "#ifdef" or "#ifndef"
    if (line.compare(0, 12, "#if defined ") == 0)
        line.erase(0, 11);
    else if (line.compare(0, 15, "#elif !defined(") == 0) {
        std::string::size_type pos = 0;

        line.erase(0, 15);
        pos = line.find(")");
        // if pos == ::npos then another part of the code will complain
        // about the mismatch
        if (pos != std::string::npos)
            line.erase(pos, 1);
    } else
        line.erase(0, line.find(" "));

    // Remove all spaces.
    std::string::size_type pos = 0;
    while ((pos = line.find(" ", pos)) != std::string::npos) {
        const unsigned char chprev(static_cast<unsigned char>((pos > 0) ? line[pos-1] : 0));
        const unsigned char chnext(static_cast<unsigned char>((pos + 1 < line.length()) ? line[pos+1] : 0));
        if ((std::isalnum(chprev) || chprev == '_') && (std::isalnum(chnext) || chnext == '_'))
            ++pos;
        else
            line.erase(pos, 1);
    }

    // The remaining string is our result.
    return line;
}



std::list<std::string> Preprocessor::getcfgs(const std::string &filedata, const std::string &filename)
{
    std::list<std::string> ret;
    ret.push_back("");

    std::list<std::string> deflist, ndeflist;

    // constants defined through "#define" in the code..
    std::set<std::string> defines;

    // How deep into included files are we currently parsing?
    // 0=>Source file, 1=>Included by source file, 2=>included by header that was included by source file, etc
    int filelevel = 0;

    bool includeguard = false;

    unsigned int linenr = 0;
    std::istringstream istr(filedata);
    std::string line;
    while (getline(istr, line)) {
        ++linenr;

        if (_errorLogger)
            _errorLogger->reportProgress(filename, "Preprocessing (get configurations 1)", 0);

        if (line.empty())
            continue;

        if (line.compare(0, 6, "#file ") == 0) {
            includeguard = true;
            ++filelevel;
            continue;
        }

        else if (line == "#endfile") {
            includeguard = false;
            if (filelevel > 0)
                --filelevel;
            continue;
        }

        if (line.compare(0, 8, "#define ") == 0) {
            bool valid = true;
            for (std::string::size_type pos = 8; pos < line.size() && line[pos] != ' '; ++pos) {
                char ch = line[pos];
                if (ch=='_' || (ch>='a' && ch<='z') || (ch>='A' && ch<='Z') || (pos>8 && ch>='0' && ch<='9'))
                    continue;
                valid = false;
                break;
            }
            if (!valid)
                line.clear();
            else if (line.find(" ", 8) == std::string::npos)
                defines.insert(line.substr(8));
            else {
                std::string s = line.substr(8);
                s[s.find(" ")] = '=';
                defines.insert(s);
            }
        }

        if (!line.empty() && line.compare(0, 3, "#if") != 0)
            includeguard = false;

        if (line.empty() || line[0] != '#')
            continue;

        if (includeguard)
            continue;

        bool from_negation = false;

        std::string def = getdef(line, true);
        if (def.empty()) {
            def = getdef(line, false);
            // sub conditionals of ndef blocks need to be
            // constructed _without_ the negated define
            if (!def.empty())
                from_negation = true;
        }
        if (!def.empty()) {
            int par = 0;
            for (std::string::size_type pos = 0; pos < def.length(); ++pos) {
                if (def[pos] == '(')
                    ++par;
                else if (def[pos] == ')') {
                    --par;
                    if (par < 0)
                        break;
                }
            }
            if (par != 0) {
                std::ostringstream lineStream;
                lineStream << __LINE__;

                ErrorLogger::ErrorMessage errmsg;
                ErrorLogger::ErrorMessage::FileLocation loc;
                loc.setfile(filename);
                loc.line = linenr;
                errmsg._callStack.push_back(loc);
                errmsg._severity = Severity::fromString("error");
                errmsg.setmsg("mismatching number of '(' and ')' in this line: " + def);
                errmsg._id  = "preprocessor" + lineStream.str();
                _errorLogger->reportErr(errmsg);
                ret.clear();
                return ret;
            }

            // Replace defined constants
            {
                std::map<std::string, std::string> varmap;
                for (std::set<std::string>::const_iterator it = defines.begin(); it != defines.end(); ++it) {
                    std::string::size_type pos = it->find("=");
                    if (pos == std::string::npos)
                        continue;
                    const std::string varname(it->substr(0, pos));
                    const std::string value(it->substr(pos + 1));
                    varmap[varname] = value;
                }

                simplifyCondition(varmap, def, false);
            }

            if (! deflist.empty() && line.compare(0, 6, "#elif ") == 0)
                deflist.pop_back();
            deflist.push_back(def);
            def = "";

            for (std::list<std::string>::const_iterator it = deflist.begin(); it != deflist.end(); ++it) {
                if (*it == "0")
                    break;
                if (*it == "1" || *it == "!")
                    continue;

                // don't add "T;T":
                // treat two and more similar nested conditions as one
                if (def != *it) {
                    if (! def.empty())
                        def += ";";
                    def += *it;
                }

                /* TODO: Fix TestPreprocessor::test7e (#2552)
                else
                {
                    std::ostringstream lineStream;
                    lineStream << __LINE__;

                    ErrorLogger::ErrorMessage errmsg;
                    ErrorLogger::ErrorMessage::FileLocation loc;
                    loc.setfile(filename);
                    loc.line = linenr;
                    errmsg._callStack.push_back(loc);
                    errmsg._severity = Severity::error;
                    errmsg.setmsg(*it+" is already guaranteed to be defined");
                    errmsg._id  = "preprocessor" + lineStream.str();
                    _errorLogger->reportErr(errmsg);
                }
                */
            }
            if (from_negation) {
                ndeflist.push_back(deflist.back());
                deflist.back() = "!";
            }

            if (std::find(ret.begin(), ret.end(), def) == ret.end()) {
                ret.push_back(def);
            }
        }

        else if (line.compare(0, 5, "#else") == 0 && ! deflist.empty()) {
            if (deflist.back() == "!") {
                deflist.back() = ndeflist.back();
                ndeflist.pop_back();
            } else {
                std::string tempDef((deflist.back() == "1") ? "0" : "1");
                deflist.back() = tempDef;
            }
        }

        else if (line.compare(0, 6, "#endif") == 0 && ! deflist.empty()) {
            if (deflist.back() == "!")
                ndeflist.pop_back();
            deflist.pop_back();
        }
    }

    // Remove defined constants from ifdef configurations..
    unsigned int count = 0;
    for (std::list<std::string>::iterator it = ret.begin(); it != ret.end(); ++it) {
        if (_errorLogger)
            _errorLogger->reportProgress(filename, "Preprocessing (get configurations 2)", (100 * count++) / ret.size());

        std::string cfg(*it);
        for (std::set<std::string>::const_iterator it2 = defines.begin(); it2 != defines.end(); ++it2) {
            std::string::size_type pos = 0;

            // Get name of define
            std::string defineName(*it2);
            if (defineName.find("=") != std::string::npos)
                defineName.erase(defineName.find("="));

            // Remove ifdef configurations that match the defineName
            while ((pos = cfg.find(defineName, pos)) != std::string::npos) {
                std::string::size_type pos1 = pos;
                ++pos;
                if (pos1 > 0 && cfg[pos1-1] != ';')
                    continue;
                std::string::size_type pos2 = pos1 + defineName.length();
                if (pos2 < cfg.length() && cfg[pos2] != ';')
                    continue;
                --pos;
                cfg.erase(pos, defineName.length());
            }
        }
        if (cfg.length() != it->length()) {
            while (cfg.length() > 0 && cfg[0] == ';')
                cfg.erase(0, 1);

            while (cfg.length() > 0 && cfg[cfg.length()-1] == ';')
                cfg.erase(cfg.length() - 1);

            std::string::size_type pos = 0;
            while ((pos = cfg.find(";;", pos)) != std::string::npos)
                cfg.erase(pos, 1);

            *it = cfg;
        }
    }

    // convert configurations: "defined(A) && defined(B)" => "A;B"
    for (std::list<std::string>::iterator it = ret.begin(); it != ret.end(); ++it) {
        std::string s(*it);

        if (s.find("&&") != std::string::npos) {
            Tokenizer tokenizer(_settings, _errorLogger);
            std::istringstream tempIstr(s);
            if (!tokenizer.tokenize(tempIstr, filename.c_str(), "", true)) {
                std::ostringstream lineStream;
                lineStream << __LINE__;

                ErrorLogger::ErrorMessage errmsg;
                ErrorLogger::ErrorMessage::FileLocation loc;
                loc.setfile(filename);
                loc.line = 1;
                errmsg._callStack.push_back(loc);
                errmsg._severity = Severity::error;
                errmsg.setmsg("Error parsing this: " + s);
                errmsg._id  = "preprocessor" + lineStream.str();
                _errorLogger->reportErr(errmsg);
            }


            const Token *tok = tokenizer.tokens();
            std::set<std::string> varList;
            while (tok) {
                if (Token::Match(tok, "defined ( %var% )")) {
                    varList.insert(tok->strAt(2));
                    tok = tok->tokAt(4);
                    if (tok && tok->str() == "&&") {
                        tok = tok->next();
                    }
                } else if (Token::Match(tok, "%var% ;")) {
                    varList.insert(tok->str());
                    tok = tok->tokAt(2);
                } else {
                    break;
                }
            }

            s = join(varList, ';');

            if (!s.empty())
                *it = s;
        }
    }

    // Convert configurations into a canonical form: B;C;A or C;A;B => A;B;C
    for (std::list<std::string>::iterator it = ret.begin(); it != ret.end(); ++it)
        *it = unify(*it, ';');

    // Remove duplicates from the ret list..
    ret.sort();
    ret.unique();

    // cleanup unhandled configurations..
    for (std::list<std::string>::iterator it = ret.begin(); it != ret.end();) {
        const std::string s(*it + ";");

        bool unhandled = false;

        for (std::string::size_type pos = 0; pos < s.length(); ++pos) {
            const unsigned char c = static_cast<unsigned char>(s[pos]);

            // ok with ";"
            if (c == ';')
                continue;

            // identifier..
            if (std::isalpha(c) || c == '_') {
                while (std::isalnum(s[pos]) || s[pos] == '_')
                    ++pos;
                if (s[pos] == '=') {
                    ++pos;
                    while (std::isdigit(s[pos]))
                        ++pos;
                    if (s[pos] != ';') {
                        unhandled = true;
                        break;
                    }
                }

                --pos;
                continue;
            }

            // not ok..
            else {
                unhandled = true;
                break;
            }
        }

        if (unhandled) {
            // unhandled ifdef configuration..
            if (_errorLogger && _settings && _settings->debugwarnings) {
                std::list<ErrorLogger::ErrorMessage::FileLocation> locationList;
                const ErrorLogger::ErrorMessage errmsg(locationList, Severity::debug, "unhandled configuration: " + *it, "debug", false);
                _errorLogger->reportErr(errmsg);
            }

            ret.erase(it++);
        } else {
            ++it;
        }
    }

    return ret;
}


void Preprocessor::simplifyCondition(const std::map<std::string, std::string> &cfg, std::string &condition, bool match)
{
    const Settings settings;
    Tokenizer tokenizer(&settings, _errorLogger);
    std::istringstream istr("(" + condition + ")");
    if (!tokenizer.tokenize(istr, "", "", true)) {
        // If tokenize returns false, then there is syntax error in the
        // code which we can't handle. So stop here.
        return;
    }

    if (Token::Match(tokenizer.tokens(), "( %var% )")) {
        std::map<std::string,std::string>::const_iterator var = cfg.find(tokenizer.tokens()->strAt(1));
        if (var != cfg.end()) {
            const std::string &value = (*var).second;
            condition = (value == "0") ? "0" : "1";
        } else if (match)
            condition = "0";
        return;
    }

    if (Token::Match(tokenizer.tokens(), "( ! %var% )")) {
        std::map<std::string,std::string>::const_iterator var = cfg.find(tokenizer.tokens()->strAt(2));

        if (var == cfg.end())
            condition = "1";
        else if (var->second == "0")
            condition = "1";
        else if (match)
            condition = "0";
        return;
    }

    // replace variable names with values..
    for (Token *tok = const_cast<Token *>(tokenizer.tokens()); tok; tok = tok->next()) {
        if (!tok->isName())
            continue;

        if (Token::Match(tok, "defined ( %var% )")) {
            if (cfg.find(tok->strAt(2)) != cfg.end())
                tok->str("1");
            else if (match)
                tok->str("0");
            else
                continue;
            tok->deleteNext(3);
            continue;
        }

        if (Token::Match(tok, "defined %var%")) {
            if (cfg.find(tok->strAt(1)) != cfg.end())
                tok->str("1");
            else if (match)
                tok->str("0");
            else
                continue;
            tok->deleteNext();
            continue;
        }

        const std::map<std::string, std::string>::const_iterator it = cfg.find(tok->str());
        if (it != cfg.end()) {
            if (!it->second.empty()) {
                // Tokenize the value
                Tokenizer tokenizer2(&settings,NULL);
                std::istringstream istr2(it->second);
                tokenizer2.tokenize(istr2,"","",true);

                // Copy the value tokens
                std::stack<Token *> link;
                for (const Token *tok2 = tokenizer2.tokens(); tok2; tok2 = tok2->next()) {
                    tok->str(tok2->str());

                    if (Token::Match(tok2,"[{([]"))
                        link.push(tok);
                    else if (!link.empty() && Token::Match(tok2,"[})]]")) {
                        Token::createMutualLinks(link.top(), tok);
                        link.pop();
                    }

                    if (tok2->next()) {
                        tok->insertToken("");
                        tok = tok->next();
                    }
                }
            } else if ((!tok->previous() || Token::Match(tok->previous(), "&&|%oror%|(")) &&
                       (!tok->next() || Token::Match(tok->next(), "&&|%oror%|)")))
                tok->str("1");
            else
                tok->deleteThis();
        }
    }

    // simplify calculations..
    bool modified = true;
    while (modified) {
        modified = false;
        modified |= tokenizer.simplifyCalculations();
        modified |= tokenizer.simplifyRedundantParenthesis();
        for (Token *tok = const_cast<Token *>(tokenizer.tokens()); tok; tok = tok->next()) {
            if (Token::Match(tok, "! %num%")) {
                tok->deleteThis();
                tok->str(tok->str() == "0" ? "1" : "0");
                modified = true;
            }
        }
    }

    for (Token *tok = const_cast<Token *>(tokenizer.tokens()); tok; tok = tok->next()) {
        if (Token::Match(tok, "(|%oror%|&& %num% &&|%oror%|)")) {
            if (tok->next()->str() != "0") {
                tok->next()->str("1");
            }
        }
    }

    for (Token *tok = const_cast<Token *>(tokenizer.tokens()); tok; tok = tok->next()) {
        while (Token::Match(tok, "(|%oror% %any% %oror% 1")) {
            tok->deleteNext(2);
            if (tok->tokAt(-3))
                tok = tok->tokAt(-3);
        }
    }

    if (Token::simpleMatch(tokenizer.tokens(), "( 1 )") ||
        Token::simpleMatch(tokenizer.tokens(), "( 1 ||"))
        condition = "1";
    else if (Token::simpleMatch(tokenizer.tokens(), "( 0 )"))
        condition = "0";
}

bool Preprocessor::match_cfg_def(const std::map<std::string, std::string> &cfg, std::string def)
{
    /*
        std::cout << "cfg: \"";
        for (std::map<std::string, std::string>::const_iterator it = cfg.begin(); it != cfg.end(); ++it)
        {
            std::cout << it->first;
            if (!it->second.empty())
                std::cout << "=" << it->second;
            std::cout << ";";
        }
        std::cout << "\"  ";
        std::cout << "def: \"" << def << "\"\n";
    */

    simplifyCondition(cfg, def, true);

    if (cfg.find(def) != cfg.end())
        return true;

    if (def == "0")
        return false;

    if (def == "1")
        return true;

    return false;
}


std::string Preprocessor::getcode(const std::string &filedata, const std::string &cfg, const std::string &filename)
{
    // For the error report
    unsigned int lineno = 0;

    std::ostringstream ret;

    bool match = true;
    std::list<bool> matching_ifdef;
    std::list<bool> matched_ifdef;

    // Create a map for the cfg for faster access to defines
    std::map<std::string, std::string> cfgmap;
    if (!cfg.empty()) {
        std::string::size_type pos = 0;
        for (;;) {
            std::string::size_type pos2 = cfg.find_first_of(";=", pos);
            if (pos2 == std::string::npos) {
                cfgmap[cfg.substr(pos)] = "";
                break;
            }
            if (cfg[pos2] == ';') {
                cfgmap[cfg.substr(pos, pos2-pos)] = "";
            } else {
                std::string::size_type pos3 = pos2;
                pos2 = cfg.find(";", pos2);
                if (pos2 == std::string::npos) {
                    cfgmap[cfg.substr(pos, pos3-pos)] = cfg.substr(pos3 + 1);
                    break;
                } else {
                    cfgmap[cfg.substr(pos, pos3-pos)] = cfg.substr(pos3 + 1, pos2 - pos3 - 1);
                }
            }
            pos = pos2 + 1;
        }
    }

    std::stack<std::string> filenames;
    filenames.push(filename);
    std::stack<unsigned int> lineNumbers;
    std::istringstream istr(filedata);
    std::string line;
    while (std::getline(istr, line)) {
        ++lineno;

        if (line.compare(0, 11, "#pragma asm") == 0) {
            ret << "\n";
            bool found_end = false;
            while (getline(istr, line)) {
                if (line.compare(0, 14, "#pragma endasm") == 0) {
                    found_end = true;
                    break;
                }

                ret << "\n";
            }
            if (!found_end)
                break;

            if (line.find("=") != std::string::npos) {
                Tokenizer tokenizer(_settings, NULL);
                line.erase(0, sizeof("#pragma endasm"));
                std::istringstream tempIstr(line);
                tokenizer.tokenize(tempIstr, "");
                if (Token::Match(tokenizer.tokens(), "( %var% = %any% )")) {
                    ret << "asm(" << tokenizer.tokens()->strAt(1) << ");";
                }
            }

            ret << "\n";

            continue;
        }

        const std::string def = getdef(line, true);
        const std::string ndef = getdef(line, false);

        const bool emptymatch = matching_ifdef.empty() | matched_ifdef.empty();

        if (line.compare(0, 8, "#define ") == 0) {
            match = true;

            if (_settings) {
                typedef std::set<std::string>::const_iterator It;
                for (It it = _settings->userUndefs.begin(); it != _settings->userUndefs.end(); ++it) {
                    std::string::size_type pos = line.find_first_not_of(' ',8);
                    if (pos != std::string::npos) {
                        std::string::size_type pos2 = line.find(*it,pos);
                        if ((pos2 != std::string::npos) &&
                            ((line.size() == pos2 + (*it).size()) ||
                             (line[pos2 + (*it).size()] == ' ') ||
                             (line[pos2 + (*it).size()] == '('))) {
                            match = false;
                            break;
                        }
                    }
                }
            }

            for (std::list<bool>::const_iterator it = matching_ifdef.begin(); it != matching_ifdef.end(); ++it)
                match &= bool(*it);

            if (match) {
                std::string::size_type pos = line.find_first_of(" (", 8);
                if (pos == std::string::npos)
                    cfgmap[line.substr(8)] = "";
                else if (line[pos] == ' ') {
                    std::string value(line.substr(pos + 1));
                    if (cfgmap.find(value) != cfgmap.end())
                        value = cfgmap[value];
                    cfgmap[line.substr(8, pos - 8)] = value;
                } else
                    cfgmap[line.substr(8, pos - 8)] = "";
            }
        }

        else if (line.compare(0, 7, "#undef ") == 0) {
            const std::string name(line.substr(7));
            cfgmap.erase(name);
        }

        else if (!emptymatch && line.compare(0, 7, "#elif !") == 0) {
            if (matched_ifdef.back()) {
                matching_ifdef.back() = false;
            } else {
                if (!match_cfg_def(cfgmap, ndef)) {
                    matching_ifdef.back() = true;
                    matched_ifdef.back() = true;
                }
            }
        }

        else if (!emptymatch && line.compare(0, 6, "#elif ") == 0) {
            if (matched_ifdef.back()) {
                matching_ifdef.back() = false;
            } else {
                if (match_cfg_def(cfgmap, def)) {
                    matching_ifdef.back() = true;
                    matched_ifdef.back() = true;
                }
            }
        }

        else if (! def.empty()) {
            matching_ifdef.push_back(match_cfg_def(cfgmap, def));
            matched_ifdef.push_back(matching_ifdef.back());
        }

        else if (! ndef.empty()) {
            matching_ifdef.push_back(! match_cfg_def(cfgmap, ndef));
            matched_ifdef.push_back(matching_ifdef.back());
        }

        else if (!emptymatch && line == "#else") {
            if (! matched_ifdef.empty())
                matching_ifdef.back() = ! matched_ifdef.back();
        }

        else if (line.compare(0, 6, "#endif") == 0) {
            if (! matched_ifdef.empty())
                matched_ifdef.pop_back();
            if (! matching_ifdef.empty())
                matching_ifdef.pop_back();
        }

        if (!line.empty() && line[0] == '#') {
            match = true;
            for (std::list<bool>::const_iterator it = matching_ifdef.begin(); it != matching_ifdef.end(); ++it)
                match &= bool(*it);
        }

        // #error => return ""
        if (match && line.compare(0, 6, "#error") == 0) {
            if (_settings && !_settings->userDefines.empty()) {
                Settings settings2(*_settings);
                Preprocessor preprocessor(&settings2, _errorLogger);
                preprocessor.error(filenames.top(), lineno, line);
            }
            return "";
        }

        if (!match && (line.compare(0, 8, "#define ") == 0 ||
                       line.compare(0, 6, "#undef") == 0)) {
            // Remove define that is not part of this configuration
            line = "";
        } else if (line.compare(0, 7, "#file \"") == 0 ||
                   line.compare(0, 8, "#endfile") == 0 ||
                   line.compare(0, 8, "#define ") == 0 ||
                   line.compare(0, 6, "#undef") == 0) {
            // We must not remove #file tags or line numbers
            // are corrupted. File tags are removed by the tokenizer.

            // Keep location info updated
            if (line.compare(0, 7, "#file \"") == 0) {
                filenames.push(line.substr(7, line.size() - 8));
                lineNumbers.push(lineno);
                lineno = 0;
            } else if (line.compare(0, 8, "#endfile") == 0) {
                if (filenames.size() > 1U)
                    filenames.pop();

                if (!lineNumbers.empty()) {
                    lineno = lineNumbers.top();
                    lineNumbers.pop();
                }
            }
        } else if (!match || line.compare(0, 1, "#") == 0) {
            // Remove #if, #else, #pragma etc, leaving only
            // #define, #undef, #file and #endfile. and also lines
            // which are not part of this configuration.
            line = "";
        }

        ret << line << "\n";
    }

    return expandMacros(ret.str(), filename, _errorLogger);
}

void Preprocessor::error(const std::string &filename, unsigned int linenr, const std::string &msg)
{
    std::list<ErrorLogger::ErrorMessage::FileLocation> locationList;
    if (!filename.empty()) {
        ErrorLogger::ErrorMessage::FileLocation loc;
        loc.line = linenr;
        loc.setfile(filename);
        locationList.push_back(loc);
    }
    _errorLogger->reportErr(ErrorLogger::ErrorMessage(locationList,
                            Severity::error,
                            msg,
                            "preprocessorErrorDirective",
                            false));
}

Preprocessor::HeaderTypes Preprocessor::getHeaderFileName(std::string &str)
{
    std::string result;
    std::string::size_type i = str.find_first_of("<\"");
    if (i == std::string::npos) {
        str = "";
        return NoHeader;
    }

    char c = str[i];
    if (c == '<')
        c = '>';

    for (i = i + 1; i < str.length(); ++i) {
        if (str[i] == c)
            break;

        result.append(1, str[i]);
    }

    // Linux can't open include paths with \ separator, so fix them
    std::replace(result.begin(), result.end(), '\\', '/');

    str = result;
    if (c == '"')
        return UserHeader;
    else
        return SystemHeader;
}

/**
 * Try to open header
 * @param filename header name (in/out)
 * @param includePaths paths where to look for the file
 * @param fin file input stream (in/out)
 * @return if file is opened then true is returned
 */
static bool openHeader(std::string &filename, const std::list<std::string> &includePaths, const std::string &filePath, std::ifstream &fin)
{
    std::list<std::string> includePaths2(includePaths);
    includePaths2.push_front("");

    for (std::list<std::string>::const_iterator iter = includePaths2.begin(); iter != includePaths2.end(); ++iter) {
        const std::string nativePath(Path::toNativeSeparators(*iter));
        fin.open((nativePath + filename).c_str());
        if (fin.is_open()) {
            filename = nativePath + filename;
            return true;
        }
        fin.clear();
    }

    fin.open((filePath + filename).c_str());
    if (fin.is_open()) {
        filename = filePath + filename;
        return true;
    }

    return false;
}


std::string Preprocessor::handleIncludes(const std::string &code, const std::string &filePath, const std::list<std::string> &includePaths, std::map<std::string,std::string> &defs, std::list<std::string> includes)
{
    const std::string path(filePath.substr(0, 1 + filePath.find_last_of("\\/")));

    // current #if indent level.
    unsigned int indent = 0;

    // how deep does the #if match? this can never be bigger than "indent".
    unsigned int indentmatch = 0;

    // has there been a true #if condition at the current indentmatch level?
    // then no more #elif or #else can be true before the #endif is seen.
    bool elseIsTrue = true;

    unsigned int linenr = 0;

    std::set<std::string> undefs = _settings ? _settings->userUndefs : std::set<std::string>();

    std::ostringstream ostr;
    std::istringstream istr(code);
    std::string line;
    bool suppressCurrentCodePath = false;
    while (std::getline(istr,line)) {
        ++linenr;

        if (line.compare(0,7,"#ifdef ") == 0) {
            if (indent == indentmatch) {
                const std::string tag = getdef(line,true);
                if (defs.find(tag) != defs.end()) {
                    elseIsTrue = false;
                    indentmatch++;
                } else if (undefs.find(tag) != undefs.end()) {
                    elseIsTrue = true;
                    indentmatch++;
                    suppressCurrentCodePath = true;
                }
            }
            ++indent;

            if (indent == indentmatch + 1)
                elseIsTrue = true;
        } else if (line.compare(0,8,"#ifndef ") == 0) {
            if (indent == indentmatch) {
                const std::string tag = getdef(line,false);
                if (defs.find(tag) == defs.end()) {
                    elseIsTrue = false;
                    indentmatch++;
                } else if (undefs.find(tag) != undefs.end()) {
                    elseIsTrue = false;
                    indentmatch++;
                    suppressCurrentCodePath = false;
                }
            }
            ++indent;

            if (indent == indentmatch + 1)
                elseIsTrue = true;

        } else if (!suppressCurrentCodePath && line.compare(0,4,"#if ") == 0) {
            if (indent == indentmatch && match_cfg_def(defs, line.substr(4))) {
                elseIsTrue = false;
                indentmatch++;
            }
            ++indent;

            if (indent == indentmatch + 1)
                elseIsTrue = true;
        } else if (line.compare(0,6,"#elif ") == 0 || line.compare(0,5,"#else") == 0) {
            if (!elseIsTrue) {
                if (indentmatch == indent) {
                    indentmatch = indent - 1;
                }
            } else {
                if (indentmatch == indent) {
                    indentmatch = indent - 1;
                } else if (indentmatch == indent - 1) {
                    if (line.compare(0,5,"#else")==0 || match_cfg_def(defs,line.substr(6))) {
                        indentmatch = indent;
                        elseIsTrue = false;
                    }
                }
            }
            if (suppressCurrentCodePath) {
                suppressCurrentCodePath = false;
                indentmatch = indent;
            }
        } else if (line.compare(0, 6, "#endif") == 0) {
            if (indent > 0)
                --indent;
            if (indentmatch > indent || indent == 0) {
                indentmatch = indent;
                elseIsTrue = false;
                suppressCurrentCodePath = false;
            }
        } else if (indentmatch == indent) {
            if (!suppressCurrentCodePath && line.compare(0, 8, "#define ") == 0) {
                const unsigned int endOfDefine = 8;
                std::string::size_type endOfTag = line.find_first_of("( ", endOfDefine);
                std::string tag;

                // define a symbol
                if (endOfTag == std::string::npos) {
                    tag = line.substr(endOfDefine);
                    defs[tag] = "";
                } else {
                    tag = line.substr(endOfDefine, endOfTag-endOfDefine);

                    // define a function-macro
                    if (line[endOfTag] == '(') {
                        defs[tag] = "";
                    }
                    // define value
                    else {
                        ++endOfTag;

                        const std::string& value = line.substr(endOfTag, line.size()-endOfTag);

                        if (defs.find(value) != defs.end())
                            defs[tag] = defs[value];
                        else
                            defs[tag] = value;
                    }
                }

                if (undefs.find(tag) != undefs.end()) {
                    defs.erase(tag);
                }
            }

            else if (!suppressCurrentCodePath && line.compare(0,7,"#undef ") == 0) {
                defs.erase(line.substr(7));
            }

            else if (!suppressCurrentCodePath && line.compare(0,7,"#error ") == 0) {
                error(filePath, linenr, line.substr(7));
            }

            else if (!suppressCurrentCodePath && line.compare(0,9,"#include ")==0) {
                std::string filename(line.substr(9));

                const HeaderTypes headerType = getHeaderFileName(filename);
                if (headerType == NoHeader) {
                    ostr << std::endl;
                    continue;
                }

                // try to open file
                std::string filepath;
                if (headerType == UserHeader)
                    filepath = path;
                std::ifstream fin;
                if (!openHeader(filename, includePaths, filepath, fin)) {

                    if (_settings && (headerType == UserHeader || _settings->debugwarnings)) {
                        if (!_settings->nomsg.isSuppressed("missingInclude", "", 0)) {
                            missingIncludeFlag = true;

                            missingInclude(Path::toNativeSeparators(filePath),
                                           linenr,
                                           filename,
                                           headerType == UserHeader);
                        }
                    }
                    ostr << std::endl;
                    continue;
                }

                // Prevent that files are recursively included
                if (std::find(includes.begin(), includes.end(), filename) != includes.end()) {
                    ostr << std::endl;
                    continue;
                }

                includes.push_back(filename);

                ostr << "#file \"" << filename << "\"\n"
                     << handleIncludes(read(fin, filename), filename, includePaths, defs, includes) << std::endl
                     << "#endfile\n";
                continue;
            }

            if (!suppressCurrentCodePath)
                ostr << line;
        }

        // A line has been read..
        ostr << "\n";
    }

    return ostr.str();
}


void Preprocessor::handleIncludes(std::string &code, const std::string &filePath, const std::list<std::string> &includePaths)
{
    std::list<std::string> paths;
    std::string path;
    path = filePath;
    path.erase(1 + path.find_last_of("\\/"));
    paths.push_back(path);
    std::string::size_type pos = 0;
    std::string::size_type endfilePos = 0;
    std::set<std::string> handledFiles;
    endfilePos = pos;
    while ((pos = code.find("#include", pos)) != std::string::npos) {
        // Accept only includes that are at the start of a line
        if (pos > 0 && code[pos-1] != '\n') {
            pos += 8; // length of "#include"
            continue;
        }

        // If endfile is encountered, we have moved to a next file in our stack,
        // so remove last path in our list.
        while ((endfilePos = code.find("\n#endfile", endfilePos)) != std::string::npos && endfilePos < pos) {
            paths.pop_back();
            endfilePos += 9; // size of #endfile
        }

        endfilePos = pos;
        std::string::size_type end = code.find("\n", pos);
        std::string filename = code.substr(pos, end - pos);

        // Remove #include clause
        code.erase(pos, end - pos);

        HeaderTypes headerType = getHeaderFileName(filename);
        if (headerType == NoHeader)
            continue;

        // filename contains now a file name e.g. "menu.h"
        std::string processedFile;
        std::string filepath;
        if (headerType == UserHeader)
            filepath = paths.back();
        std::ifstream fin;
        const bool fileOpened(openHeader(filename, includePaths, filepath, fin));

        if (fileOpened) {
            filename = Path::simplifyPath(filename.c_str());
            std::string tempFile = filename;
            std::transform(tempFile.begin(), tempFile.end(), tempFile.begin(), tolowerWrapper);
            if (handledFiles.find(tempFile) != handledFiles.end()) {
                // We have processed this file already once, skip
                // it this time to avoid eternal loop.
                fin.close();
                continue;
            }

            handledFiles.insert(tempFile);
            processedFile = Preprocessor::read(fin, filename);
            fin.close();
        }

        if (!processedFile.empty()) {
            // Remove space characters that are after or before new line character
            processedFile = "#file \"" + filename + "\"\n" + processedFile + "\n#endfile";
            code.insert(pos, processedFile);

            path = filename;
            path.erase(1 + path.find_last_of("\\/"));
            paths.push_back(path);
        } else if (!fileOpened && _settings && (headerType == UserHeader || _settings->debugwarnings)) {
            if (!_settings->nomsg.isSuppressed("missingInclude", "", 0))
                missingIncludeFlag = true;

            if (_errorLogger && _settings->checkConfiguration) {
                std::string f = filePath;

                // Determine line number of include
                unsigned int linenr = 1;
                unsigned int level = 0;
                for (std::string::size_type p = 1; p <= pos; ++p) {
                    if (level == 0 && code[pos-p] == '\n')
                        ++linenr;
                    else if (code.compare(pos-p, 9, "#endfile\n") == 0) {
                        ++level;
                    } else if (code.compare(pos-p, 6, "#file ") == 0) {
                        if (level == 0) {
                            linenr--;
                            const std::string::size_type pos1 = pos - p + 7;
                            const std::string::size_type pos2 = code.find_first_of("\"\n", pos1);
                            f = code.substr(pos1, (pos2 == std::string::npos) ? pos2 : (pos2 - pos1));
                            break;
                        }
                        --level;
                    }
                }

                if (!_settings->nomsg.isSuppressed("missingInclude", f, linenr)) {
                    missingInclude(Path::toNativeSeparators(f),
                                   linenr,
                                   filename,
                                   headerType == UserHeader);
                }
            }
        }
    }
}

// Report that include is missing
void Preprocessor::missingInclude(const std::string &filename, unsigned int linenr, const std::string &header, bool userheader)
{
    std::list<ErrorLogger::ErrorMessage::FileLocation> locationList;
    if (!filename.empty()) {
        ErrorLogger::ErrorMessage::FileLocation loc;
        loc.line = linenr;
        loc.setfile(filename);
        locationList.push_back(loc);
    }

    // If the missing include is a system header then this is
    // currently a debug-message.
    const Severity::SeverityType severity = userheader ? Severity::information : Severity::debug;
    const std::string id = userheader ? "missingInclude" : "debug";
    ErrorLogger::ErrorMessage errmsg(locationList, severity, "Include file: \"" + header + "\" not found.", id, false);
    errmsg.file0 = file0;
    _errorLogger->reportErr(errmsg);
}

/**
 * Skip string in line. A string begins and ends with either a &quot; or a &apos;
 * @param line the string
 * @param pos in=start position of string, out=end position of string
 */
static void skipstring(const std::string &line, std::string::size_type &pos)
{
    const char ch = line[pos];

    ++pos;
    while (pos < line.size() && line[pos] != ch) {
        if (line[pos] == '\\')
            ++pos;
        ++pos;
    }
}

/**
 * @brief get parameters from code. For example 'foo(1,2)' => '1','2'
 * @param line in: The code
 * @param pos  in: Position to the '('. out: Position to the ')'
 * @param params out: The extracted parameters
 * @param numberOfNewlines out: number of newlines in the macro call
 * @param endFound out: was the end parenthesis found?
 */
static void getparams(const std::string &line,
                      std::string::size_type &pos,
                      std::vector<std::string> &params,
                      unsigned int &numberOfNewlines,
                      bool &endFound)
{
    params.clear();
    numberOfNewlines = 0;
    endFound = false;

    if (line[pos] == ' ')
        pos++;

    if (line[pos] != '(')
        return;

    // parentheses level
    int parlevel = 0;

    // current parameter data
    std::string par;

    // scan for parameters..
    for (; pos < line.length(); ++pos) {
        // increase parenthesis level
        if (line[pos] == '(') {
            ++parlevel;
            if (parlevel == 1)
                continue;
        }

        // decrease parenthesis level
        else if (line[pos] == ')') {
            --parlevel;
            if (parlevel <= 0) {
                endFound = true;
                params.push_back(par);
                break;
            }
        }

        // string
        else if (line[pos] == '\"' || line[pos] == '\'') {
            const std::string::size_type p = pos;
            skipstring(line, pos);
            if (pos == line.length())
                break;
            par += line.substr(p, pos + 1 - p);
            continue;
        }

        // count newlines. the expanded macro must have the same number of newlines
        else if (line[pos] == '\n') {
            ++numberOfNewlines;
            continue;
        }

        // new parameter
        if (parlevel == 1 && line[pos] == ',') {
            params.push_back(par);
            par = "";
        }

        // spaces are only added if needed
        else if (line[pos] == ' ') {
            // Add space only if it is needed
            if (par.size() && std::isalnum(par[par.length()-1])) {
                par += ' ';
            }
        }

        // add character to current parameter
        else if (parlevel >= 1) {
            par.append(1, line[pos]);
        }
    }
}

/** @brief Class that the preprocessor uses when it expands macros. This class represents a preprocessor macro */
class PreprocessorMacro {
private:
    Settings settings;

    /** tokens of this macro */
    Tokenizer tokenizer;

    /** macro parameters */
    std::vector<std::string> _params;

    /** name of macro */
    std::string _name;

    /** macro definition in plain text */
    const std::string _macro;

    /** does this macro take a variable number of parameters? */
    bool _variadic;

    /** prefix that is used by cppcheck to separate macro parameters. Always "__cppcheck__" */
    const std::string _prefix;

    /** The macro has parentheses but no parameters.. "AAA()" */
    bool _nopar;

    /** disabled assignment operator */
    void operator=(const PreprocessorMacro &);

    /** @brief expand inner macro */
    std::vector<std::string> expandInnerMacros(const std::vector<std::string> &params1,
            const std::map<std::string, PreprocessorMacro *> &macros) const {
        std::string innerMacroName;

        // Is there an inner macro..
        {
            const Token *tok = Token::findsimplematch(tokens(), ")");
            if (!Token::Match(tok, ") %var% ("))
                return params1;
            innerMacroName = tok->strAt(1);
            tok = tok->tokAt(3);
            unsigned int par = 0;
            while (Token::Match(tok, "%var% ,|)")) {
                tok = tok->tokAt(2);
                par++;
            }
            if (tok || par != params1.size())
                return params1;
        }

        std::vector<std::string> params2(params1);

        for (unsigned int ipar = 0; ipar < params1.size(); ++ipar) {
            const std::string s(innerMacroName + "(");
            std::string param(params1[ipar]);
            if (param.compare(0,s.length(),s)==0 && param[param.length()-1]==')') {
                std::vector<std::string> innerparams;
                std::string::size_type pos = s.length() - 1;
                unsigned int num = 0;
                bool endFound = false;
                getparams(param, pos, innerparams, num, endFound);
                if (pos == param.length()-1 && num==0 && endFound && innerparams.size() == params1.size()) {
                    // Is inner macro defined?
                    std::map<std::string, PreprocessorMacro *>::const_iterator it = macros.find(innerMacroName);
                    if (it != macros.end()) {
                        // expand the inner macro
                        const PreprocessorMacro *innerMacro = it->second;

                        std::string innercode;
                        std::map<std::string,PreprocessorMacro *> innermacros = macros;
                        innermacros.erase(innerMacroName);
                        innerMacro->code(innerparams, innermacros, innercode);
                        params2[ipar] = innercode;
                    }
                }
            }
        }

        return params2;
    }

public:
    /**
     * @brief Constructor for PreprocessorMacro. This is the "setter"
     * for this class - everything is setup here.
     * @param macro The code after define, until end of line,
     * e.g. "A(x) foo(x);"
     */
    explicit PreprocessorMacro(const std::string &macro)
        : _macro(macro), _prefix("__cppcheck__") {
        tokenizer.setSettings(&settings);

        // Tokenize the macro to make it easier to handle
        std::istringstream istr(macro);
        tokenizer.createTokens(istr);

        // macro name..
        if (tokens() && tokens()->isName())
            _name = tokens()->str();

        // initialize parameters to default values
        _variadic = _nopar = false;

        std::string::size_type pos = macro.find_first_of(" (");
        if (pos != std::string::npos && macro[pos] == '(') {
            // Extract macro parameters
            if (Token::Match(tokens(), "%var% ( %var%")) {
                for (const Token *tok = tokens()->tokAt(2); tok; tok = tok->next()) {
                    if (tok->str() == ")")
                        break;
                    if (Token::simpleMatch(tok, ". . . )")) {
                        if (tok->previous()->str() == ",")
                            _params.push_back("__VA_ARGS__");
                        _variadic = true;
                        break;
                    }
                    if (tok->isName())
                        _params.push_back(tok->str());
                }
            }

            else if (Token::Match(tokens(), "%var% ( . . . )"))
                _variadic = true;

            else if (Token::Match(tokens(), "%var% ( )"))
                _nopar = true;
        }
    }

    /** return tokens of this macro */
    const Token *tokens() const {
        return tokenizer.tokens();
    }

    /** read parameters of this macro */
    const std::vector<std::string> &params() const {
        return _params;
    }

    /** check if this is macro has a variable number of parameters */
    bool variadic() const {
        return _variadic;
    }

    /** Check if this macro has parentheses but no parameters */
    bool nopar() const {
        return _nopar;
    }

    /** name of macro */
    const std::string &name() const {
        return _name;
    }

    /**
     * get expanded code for this macro
     * @param params2 macro parameters
     * @param macros macro definitions (recursion)
     * @param macrocode output string
     * @return true if the expanding was successful
     */
    bool code(const std::vector<std::string> &params2, const std::map<std::string, PreprocessorMacro *> &macros, std::string &macrocode) const {
        if (_nopar || (_params.empty() && _variadic)) {
            macrocode = _macro.substr(1 + _macro.find(")"));
            if (macrocode.empty())
                return true;

            std::string::size_type pos = 0;
            // Remove leading spaces
            if ((pos = macrocode.find_first_not_of(" ")) > 0)
                macrocode.erase(0, pos);
            // Remove ending newline
            if ((pos = macrocode.find_first_of("\r\n")) != std::string::npos)
                macrocode.erase(pos);

            // Replace "__VA_ARGS__" with parameters
            if (!_nopar) {
                std::string s;
                for (unsigned int i = 0; i < params2.size(); ++i) {
                    if (i > 0)
                        s += ",";
                    s += params2[i];
                }

                pos = 0;
                while ((pos = macrocode.find("__VA_ARGS__", pos)) != std::string::npos) {
                    macrocode.erase(pos, 11);
                    macrocode.insert(pos, s);
                    pos += s.length();
                }
            }
        }

        else if (_params.empty()) {
            std::string::size_type pos = _macro.find_first_of(" \"");
            if (pos == std::string::npos)
                macrocode = "";
            else {
                if (_macro[pos] == ' ')
                    pos++;
                macrocode = _macro.substr(pos);
                if ((pos = macrocode.find_first_of("\r\n")) != std::string::npos)
                    macrocode.erase(pos);
            }
        }

        else {
            const std::vector<std::string> givenparams = expandInnerMacros(params2, macros);

            const Token *tok = tokens();
            while (tok && tok->str() != ")")
                tok = tok->next();
            if (tok) {
                bool optcomma = false;
                while (NULL != (tok = tok->next())) {
                    std::string str = tok->str();
                    if (str == "##")
                        continue;
                    if (str[0] == '#' || tok->isName()) {
                        const bool stringify(str[0] == '#');
                        if (stringify) {
                            str = str.erase(0, 1);
                        }
                        for (unsigned int i = 0; i < _params.size(); ++i) {
                            if (str == _params[i]) {
                                if (_variadic &&
                                    (i == _params.size() - 1 ||
                                     (givenparams.size() + 2 == _params.size() && i + 1 == _params.size() - 1))) {
                                    str = "";
                                    for (unsigned int j = (unsigned int)_params.size() - 1; j < givenparams.size(); ++j) {
                                        if (optcomma || j > _params.size() - 1)
                                            str += ",";
                                        optcomma = false;
                                        str += givenparams[j];
                                    }
                                } else if (i >= givenparams.size()) {
                                    // Macro had more parameters than caller used.
                                    macrocode = "";
                                    return false;
                                } else if (stringify) {
                                    const std::string &s(givenparams[i]);
                                    std::ostringstream ostr;
                                    ostr << "\"";
                                    for (std::string::size_type j = 0; j < s.size(); ++j) {
                                        if (s[j] == '\\' || s[j] == '\"')
                                            ostr << '\\';
                                        ostr << s[j];
                                    }
                                    str = ostr.str() + "\"";
                                } else
                                    str = givenparams[i];

                                break;
                            }
                        }

                        // expand nopar macro
                        if (tok->strAt(-1) != "##") {
                            const std::map<std::string, PreprocessorMacro *>::const_iterator it = macros.find(str);
                            if (it != macros.end() && it->second->_macro.find("(") == std::string::npos) {
                                str = it->second->_macro;
                                if (str.find(" ") != std::string::npos)
                                    str.erase(0, str.find(" "));
                                else
                                    str = "";
                            }
                        }
                    }
                    if (_variadic && tok->str() == "," && tok->next() && tok->next()->str() == "##") {
                        optcomma = true;
                        continue;
                    }
                    optcomma = false;
                    macrocode += str;
                    if (Token::Match(tok, "%var% %var%") ||
                        Token::Match(tok, "%var% %num%") ||
                        Token::Match(tok, "%num% %var%") ||
                        Token::simpleMatch(tok, "> >"))
                        macrocode += " ";
                }
            }
        }

        return true;
    }
};

/**
 * Get data from a input string. This is an extended version of std::getline.
 * The std::getline only get a single line at a time. It can therefore happen that it
 * contains a partial statement. This function ensures that the returned data
 * doesn't end in the middle of a statement. The "getlines" name indicate that
 * this function will return multiple lines if needed.
 * @param istr input stream
 * @param line output data
 * @return success
 */
static bool getlines(std::istream &istr, std::string &line)
{
    if (!istr.good())
        return false;
    line = "";
    int parlevel = 0;
    for (char ch = (char)istr.get(); istr.good(); ch = (char)istr.get()) {
        if (ch == '\'' || ch == '\"') {
            line += ch;
            char c = 0;
            while (istr.good() && c != ch) {
                if (c == '\\') {
                    c = (char)istr.get();
                    if (!istr.good())
                        return true;
                    line += c;
                }

                c = (char)istr.get();
                if (!istr.good())
                    return true;
                if (c == '\n' && line.compare(0, 1, "#") == 0)
                    return true;
                line += c;
            }
            continue;
        }
        if (ch == '(')
            ++parlevel;
        else if (ch == ')')
            --parlevel;
        else if (ch == '\n') {
            if (line.compare(0, 1, "#") == 0)
                return true;

            if (istr.peek() == '#') {
                line += ch;
                return true;
            }
        } else if (line.compare(0, 1, "#") != 0 && parlevel <= 0 && ch == ';') {
            line += ";";
            return true;
        }

        line += ch;
    }
    return true;
}

std::string Preprocessor::expandMacros(const std::string &code, std::string filename, ErrorLogger *errorLogger)
{
    // Search for macros and expand them..
    // --------------------------------------------

    // Available macros (key=macroname, value=macro).
    std::map<std::string, PreprocessorMacro *> macros;

    // Current line number
    unsigned int linenr = 1;

    // linenr, filename
    std::stack< std::pair<unsigned int, std::string> > fileinfo;

    // output stream
    std::ostringstream ostr;

    // read code..
    std::istringstream istr(code);
    std::string line;
    while (getlines(istr, line)) {
        // defining a macro..
        if (line.compare(0, 8, "#define ") == 0) {
            PreprocessorMacro *macro = new PreprocessorMacro(line.substr(8));
            if (macro->name().empty()) {
                delete macro;
            } else if (macro->name() == "BOOST_FOREACH") {
                // BOOST_FOREACH is currently too complex to parse, so skip it.
                delete macro;
            } else {
                std::map<std::string, PreprocessorMacro *>::iterator it;
                it = macros.find(macro->name());
                if (it != macros.end())
                    delete it->second;
                macros[macro->name()] = macro;
            }
            line = "\n";
        }

        // undefining a macro..
        else if (line.compare(0, 7, "#undef ") == 0) {
            std::map<std::string, PreprocessorMacro *>::iterator it;
            it = macros.find(line.substr(7));
            if (it != macros.end()) {
                delete it->second;
                macros.erase(it);
            }
            line = "\n";
        }

        // entering a file, update position..
        else if (line.compare(0, 7, "#file \"") == 0) {
            fileinfo.push(std::pair<unsigned int, std::string>(linenr, filename));
            filename = line.substr(7, line.length() - 8);
            linenr = 0;
            line += "\n";
        }

        // leaving a file, update position..
        else if (line == "#endfile") {
            if (!fileinfo.empty()) {
                linenr = fileinfo.top().first;
                filename = fileinfo.top().second;
                fileinfo.pop();
            }
            line += "\n";
        }

        // all other preprocessor directives are just replaced with a newline
        else if (line.compare(0, 1, "#") == 0) {
            line += "\n";
        }

        // expand macros..
        else {
            // Limit for each macro.
            // The limit specify a position in the "line" variable.
            // For a "recursive macro" where the expanded text contains
            // the macro again, the macro should not be expanded again.
            // The limits are used to prevent recursive expanding.
            // * When a macro is expanded its limit position is set to
            //   the last expanded character.
            // * macros are only allowed to be expanded when the
            //   the position is beyond the limit.
            // * The limit is relative to the end of the "line"
            //   variable. Inserting and deleting text before the limit
            //   without updating the limit is safe.
            // * when pos goes beyond a limit the limit needs to be
            //   deleted because it is unsafe to insert/delete text
            //   after the limit otherwise
            std::map<const PreprocessorMacro *, unsigned int> limits;

            // pos is the current position in line
            std::string::size_type pos = 0;

            // scan line to see if there are any macros to expand..
            unsigned int tmpLinenr = 0;
            while (pos < line.size()) {
                if (line[pos] == '\n')
                    ++tmpLinenr;

                // skip strings..
                if (line[pos] == '\"' || line[pos] == '\'') {
                    const char ch = line[pos];

                    skipstring(line, pos);
                    ++pos;

                    if (pos >= line.size()) {
                        writeError(filename,
                                   linenr + tmpLinenr,
                                   errorLogger,
                                   "noQuoteCharPair",
                                   std::string("No pair for character (") + ch + "). Can't process file. File is either invalid or unicode, which is currently not supported.");

                        std::map<std::string, PreprocessorMacro *>::iterator it;
                        for (it = macros.begin(); it != macros.end(); ++it)
                            delete it->second;
                        macros.clear();
                        return "";
                    }

                    continue;
                }

                if (!std::isalpha(line[pos]) && line[pos] != '_')
                    ++pos;

                // found an identifier..
                // the "while" is used in case the expanded macro will immediately call another macro
                while (pos < line.length() && (std::isalpha(line[pos]) || line[pos] == '_')) {
                    // pos1 = start position of macro
                    const std::string::size_type pos1 = pos++;

                    // find the end of the identifier
                    while (pos < line.size() && (std::isalnum(line[pos]) || line[pos] == '_'))
                        ++pos;

                    // get identifier
                    const std::string id = line.substr(pos1, pos - pos1);

                    // is there a macro with this name?
                    std::map<std::string, PreprocessorMacro *>::const_iterator it;
                    it = macros.find(id);
                    if (it == macros.end())
                        break;  // no macro with this name exist

                    const PreprocessorMacro * const macro = it->second;

                    // check that pos is within allowed limits for this
                    // macro
                    {
                        const std::map<const PreprocessorMacro *, unsigned int>::const_iterator it2 = limits.find(macro);
                        if (it2 != limits.end() && pos <= line.length() - it2->second)
                            break;
                    }

                    // get parameters from line..
                    std::vector<std::string> params;
                    std::string::size_type pos2 = pos;
                    if (macro->params().size() && pos2 >= line.length())
                        break;

                    // number of newlines within macro use
                    unsigned int numberOfNewlines = 0;

                    // if the macro has parentheses, get parameters
                    if (macro->variadic() || macro->nopar() || macro->params().size()) {
                        // is the end parenthesis found?
                        bool endFound = false;

                        getparams(line,pos2,params,numberOfNewlines,endFound);


                        // something went wrong so bail out
                        if (!endFound)
                            break;
                    }

                    // Just an empty parameter => clear
                    if (params.size() == 1 && params[0] == "")
                        params.clear();

                    // Check that it's the same number of parameters..
                    if (!macro->variadic() && params.size() != macro->params().size())
                        break;

                    // Create macro code..
                    std::string tempMacro;
                    if (!macro->code(params, macros, tempMacro)) {
                        // Syntax error in code
                        writeError(filename,
                                   linenr + tmpLinenr,
                                   errorLogger,
                                   "syntaxError",
                                   std::string("Syntax error. Not enough parameters for macro '") + macro->name() + "'.");

                        std::map<std::string, PreprocessorMacro *>::iterator iter;
                        for (iter = macros.begin(); iter != macros.end(); ++iter)
                            delete iter->second;
                        macros.clear();
                        return "";
                    }

                    // make sure number of newlines remain the same..
                    std::string macrocode(std::string(numberOfNewlines, '\n') + tempMacro);

                    // Insert macro code..
                    if (macro->variadic() || macro->nopar() || !macro->params().empty())
                        ++pos2;

                    // Remove old limits
                    for (std::map<const PreprocessorMacro *, unsigned int>::iterator iter = limits.begin();
                         iter != limits.end();) {
                        if ((line.length() - pos1) < iter->second) {
                            // We have gone past this limit, so just delete it
                            limits.erase(iter++);
                        } else {
                            ++iter;
                        }
                    }

                    // don't allow this macro to be expanded again before pos2
                    limits[macro] = line.length() - pos2;

                    // erase macro
                    line.erase(pos1, pos2 - pos1);

                    // Don't glue this macro into variable or number after it
                    if (std::isalnum(line[pos1]) || line[pos1] == '_')
                        macrocode.append(1,' ');

                    // insert expanded macro code
                    line.insert(pos1, "$" + macrocode);

                    // position = start position.
                    pos = pos1;
                }
            }
        }

        // the line has been processed in various ways. Now add it to the output stream
        ostr << line;

        // update linenr
        for (std::string::size_type p = 0; p < line.length(); ++p) {
            if (line[p] == '\n')
                ++linenr;
        }
    }

    for (std::map<std::string, PreprocessorMacro *>::iterator it = macros.begin(); it != macros.end(); ++it)
        delete it->second;
    macros.clear();

    return ostr.str();
}


void Preprocessor::getErrorMessages(ErrorLogger *errorLogger, const Settings *settings)
{
    Settings settings2(*settings);
    Preprocessor preprocessor(&settings2, errorLogger);
    preprocessor.missingInclude("", 1, "", true);
    preprocessor.error("", 1, "#error message");   // #error ..
}
