CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -pthread
TARGET   := procmon
SRC      := procmon.cpp

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $<
	@echo ""
	@echo "  ✓  Build successful → ./$(TARGET)"
	@echo ""

run: all
	./$(TARGET)

clean:
	rm -f $(TARGET)
