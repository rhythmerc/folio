#pragma once

#include <cassert>
#include <cstdint>

class GridHelper {
  private:
    uint16_t itemCount;
    uint8_t cols;
    uint8_t rowsPerPage;
    uint16_t index;

  public: 
    GridHelper(uint16_t itemCount, uint8_t rowsPerPage, uint8_t cols, uint16_t index): 
      itemCount(itemCount), 
      rowsPerPage(rowsPerPage),
      cols(cols), 
      index(0)
      {
        setByIndex(index);
      };

    uint16_t rowForIndex(uint16_t index);
    uint8_t colForIndex(uint16_t index);

    uint8_t currentRow();
    uint8_t currentCol();
    uint8_t currentPage();

    void nextItem();

    void up();
    void down();
    void left();
    void right();

    void setByRowColPage(uint8_t row, uint8_t col, uint8_t page);
    void setByIndex(uint16_t index); 

    uint8_t itemsPerPage();
    uint8_t pageCount();
    uint8_t rowsOnFinalPage();

    uint16_t currentIndex() { return this->index; }
    uint16_t currentIndexOnPage() { return this->index % this->itemsPerPage(); }
};
