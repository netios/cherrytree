/*
 * ct_table_light.cc
 *
 * Copyright 2009-2024
 * Giuseppe Penone <giuspen@gmail.com>
 * Evgenii Gurianov <https://github.com/txe>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */

#include "ct_table.h"
#include "ct_main_win.h"
#include "ct_actions.h"
#include "ct_storage_sqlite.h"
#include "ct_storage_xml.h"
#include "ct_logging.h"
#include "ct_misc_utils.h"

/*static*/void CtTableLight::_free_matrix(CtTableMatrix& tableMatrix)
{
    for (CtTableRow& tableRow : tableMatrix) {
        for (void* pText : tableRow) {
            delete static_cast<Glib::ustring*>(pText);
            pText = nullptr;
        }
    }
}

CtTableLight::CtTableLight(CtMainWin* pCtMainWin,
                           CtTableMatrix& tableMatrix,
                           const int colWidthDefault,
                           const int charOffset,
                           const std::string& justification,
                           const CtTableColWidths& colWidths,
                           const size_t currRow,
                           const size_t currCol)
 : CtTableCommon{pCtMainWin, colWidthDefault, charOffset, justification, colWidths, currRow, currCol}
{
    _reset(tableMatrix);
}

void CtTableLight::_reset(CtTableMatrix& tableMatrix)
{
    const size_t numRows{tableMatrix.size()};
    size_t numColumns{0u};
    // enforce same number of columns per row
    for (size_t r = 0u; r < numRows; ++r) {
        if (tableMatrix[r].size() > numColumns) { numColumns = tableMatrix[r].size(); }
    }
    for (size_t r = 0u; r < numRows; ++r) {
        while (tableMatrix[r].size() < numColumns) { tableMatrix[r].push_back(new Glib::ustring{}); }
    }

    // column widths can be empty or wrong, trying to fix it
    // so we don't need to check it again and again
    while (_colWidths.size() < numColumns) {
        _colWidths.push_back(0); // 0 means we use default width
    }

    _pColumns.reset(new CtTableLightColumns{numColumns});
    _pListStore = Gtk::ListStore::create(*_pColumns);

    for (size_t r = 0u; r < numRows; ++r) {
        Gtk::TreeModel::Row row = *(_pListStore->append());
        row[_pColumns->columnWeight] = CtTreeIter::get_pango_weight_from_is_bold(0u == r);
        for (size_t c = 0u; c < numColumns; ++c) {
            row[_pColumns->columnsText.at(c)] = *static_cast<Glib::ustring*>(tableMatrix.at(r).at(c));
        }
    }
    CtTableLight::_free_matrix(tableMatrix);

    if (_pManagedTreeView) {
        _frame.remove();
        delete _pManagedTreeView;
    }
    _pManagedTreeView = Gtk::manage(new Gtk::TreeView{_pListStore});
    _pManagedTreeView->set_headers_visible(false);
    _pManagedTreeView->set_grid_lines(Gtk::TreeViewGridLines::TREE_VIEW_GRID_LINES_BOTH);
    _pManagedTreeView->get_selection()->set_mode(Gtk::SelectionMode::SELECTION_NONE);
    for (size_t c = 0u; c < numColumns; ++c) {
        const int width = get_col_width(c);
        _pManagedTreeView->append_column_editable(""/*header*/, _pColumns->columnsText.at(c));
        Gtk::TreeViewColumn* pTVColumn = _pManagedTreeView->get_column(c);
        if (pTVColumn) {
            auto pCellRendererText = static_cast<Gtk::CellRendererText*>(pTVColumn->get_first_cell());
            pTVColumn->add_attribute(pCellRendererText->property_weight(), _pColumns->columnWeight);
            pCellRendererText->property_wrap_width() = width;
            pCellRendererText->property_wrap_mode() = Pango::WrapMode::WRAP_WORD_CHAR;
            pTVColumn->property_sizing() = Gtk::TREE_VIEW_COLUMN_AUTOSIZE;
            pTVColumn->property_min_width() = width/2;
            pCellRendererText->signal_edited().connect(sigc::bind<size_t>(sigc::mem_fun(*this, &CtTableLight::_on_cell_renderer_text_edited), c), false);
            pCellRendererText->signal_editing_started().connect(sigc::bind<size_t>(sigc::mem_fun(*this, &CtTableLight::_on_cell_renderer_editing_started), c), false);
        }
    }
    _pManagedTreeView->signal_button_press_event().connect(sigc::mem_fun(*this, &CtTableCommon::on_table_button_press_event), false);
    _pManagedTreeView->signal_event_after().connect(sigc::mem_fun(*this, &CtTableLight::_on_treeview_event_after));

    _pManagedTreeView->get_style_context()->add_class("ct-table-light");

    _frame.add(*_pManagedTreeView);
    show_all();
}

void CtTableLight::_on_cell_renderer_text_edited(const Glib::ustring& path, const Glib::ustring& new_text, const size_t column)
{
    Gtk::TreeIter treeIter{_pListStore->get_iter(path)};
    if (treeIter) {
        Gtk::TreeRow treeRow{*treeIter};
        if (treeRow[_pColumns->columnsText.at(column)] != new_text) {
            treeRow[_pColumns->columnsText.at(column)] = new_text;
            _pCtMainWin->update_window_save_needed(CtSaveNeededUpdType::nbuf, true/*new_machine_state*/);
        }
    }
    _pEditingCellEntry = nullptr;
}

void CtTableLight::_on_cell_renderer_editing_started(Gtk::CellEditable* editable, const Glib::ustring& path, const size_t column)
{
    _pEditingCellEntry = dynamic_cast<Gtk::Entry*>(editable);
    if (_pEditingCellEntry) {
        _currentRow = std::stoi(path.raw());
        _currentColumn = column;
        _pEditingCellEntry->signal_populate_popup().connect(sigc::mem_fun(*this, &CtTableCommon::on_cell_populate_popup));
        _pEditingCellEntry->signal_key_press_event().connect(sigc::mem_fun(*this, &CtTableCommon::on_cell_key_press_event), false);
        _pEditingCellEntry->signal_focus_out_event().connect(sigc::bind(sigc::mem_fun(*this, &CtTableLight::_on_entry_focus_out_event), _pEditingCellEntry, path, column));
    }
}

bool CtTableLight::_on_entry_focus_out_event(GdkEventFocus*/*gdk_event*/, Gtk::Entry* pEntry, const Glib::ustring& path, const size_t column)
{
    _on_cell_renderer_text_edited(path, pEntry->get_text(), column);
    _pEditingCellEntry = nullptr;
    return false;
}

void CtTableLight::write_strings_matrix(std::vector<std::vector<Glib::ustring>>& rows) const
{
    rows.reserve(get_num_rows());
    auto f_action = [&](const Gtk::TreeIter& treeIter)->bool{
        Gtk::TreeRow treeRow = *treeIter;
        rows.push_back(std::vector<Glib::ustring>{});
        const size_t numCols = get_num_columns();
        rows.back().reserve(numCols);
        const CtTableLightColumns& cols = get_columns();
        for (size_t c = 0u; c < numCols; ++c) {
            rows.back().push_back(treeRow[cols.columnsText.at(c)]);
        }
        return false; /* to continue */
    };
    _pListStore->foreach_iter(f_action);
}

void CtTableLight::_populate_xml_rows_cells(xmlpp::Element* p_table_node) const
{
    // put header at the end
    Gtk::TreeIter headerIter;
    bool is_header{true};
    auto f_action = [&](const Gtk::TreeIter& treeIter)->bool{
        if (is_header) {
            is_header = false;
            headerIter = treeIter;
        }
        else {
            Gtk::TreeRow treeRow = *treeIter;
            xmlpp::Element* p_row_node = p_table_node->add_child("row");
            const size_t numCols = get_num_columns();
            const CtTableLightColumns& cols = get_columns();
            for (size_t c = 0u; c < numCols; ++c) {
                xmlpp::Element* p_cell_node = p_row_node->add_child("cell");
                p_cell_node->add_child_text(treeRow[cols.columnsText.at(c)]);
            }
        }
        return false; /* to continue */
    };
    _pListStore->foreach_iter(f_action);
    if (not is_header) f_action(headerIter);
}

std::shared_ptr<CtAnchoredWidgetState> CtTableLight::get_state()
{
    return std::shared_ptr<CtAnchoredWidgetState>(new CtAnchoredWidgetState_TableLight{this});
}

void CtTableLight::row_add(const size_t afterRowIdx, const std::vector<Glib::ustring>* pNewRow/*= nullptr*/)
{
    Gtk::TreeIter afterIter = _pListStore->get_iter(Gtk::TreePath{std::to_string(afterRowIdx)});
    Gtk::TreeIter newIter;
    if (afterIter) {
        newIter = _pListStore->insert_after(afterIter);
    }
    else {
        newIter = _pListStore->append();
    }
    Gtk::TreeModel::Row newRow = *newIter;
    newRow[_pColumns->columnWeight] = CtTreeIter::get_pango_weight_from_is_bold(false);
    if (pNewRow) {
        const size_t numColsTo = get_num_columns();
        const size_t numColsFrom = pNewRow->size();
        for (size_t c = 0u; c < numColsTo and c < numColsFrom; ++c) {
            newRow[_pColumns->columnsText.at(c)] = pNewRow->at(c);
        }
    }
    grab_focus();
}

void CtTableLight::row_delete(const size_t rowIdx)
{
    if (1u == get_num_rows() or rowIdx >= get_num_rows()) {
        return;
    }
    Gtk::TreePath treePath{std::to_string(rowIdx)};
    Gtk::TreeIter treeIter = _pListStore->get_iter(treePath);
    if (not treeIter) {
        return;
    }
    //const CtTableLightColumns& cols = get_columns();
    //spdlog::debug("rm row {} [{},{},{},{}]", treePath.to_string().raw(),
    //    treeIter->get_value(cols.columnsText.at(0)),
    //    treeIter->get_value(cols.columnsText.at(1)),
    //    treeIter->get_value(cols.columnsText.at(2)),
    //    treeIter->get_value(cols.columnsText.at(3)));
    exit_cell_edit();
    (void)_pListStore->erase(treeIter);
    if (_currentRow == get_num_rows()) {
        --_currentRow;
    }
    if (0u == rowIdx) {
        // we deleted the header
        Gtk::TreeIter treeIterHeader = _pListStore->get_iter(treePath);
        if (treeIterHeader) {
            (*treeIterHeader)[_pColumns->columnWeight] = CtTreeIter::get_pango_weight_from_is_bold(true);
        }
    }
    grab_focus();
}

void CtTableLight::row_move_up(const size_t rowIdx, const bool from_move_down)
{
    if (0u == rowIdx) {
        return;
    }
    const size_t rowIdxUp = rowIdx - 1u;
    Gtk::TreePath treePath{std::to_string(rowIdx)};
    Gtk::TreeIter treeIter = _pListStore->get_iter(treePath);
    Gtk::TreeIter treeIterUp = _pListStore->get_iter(Gtk::TreePath{std::to_string(rowIdxUp)});
    if (not treeIter or not treeIterUp) {
        return;
    }
    if (0u == rowIdxUp) {
        // we are swapping header
        (*treeIter)[_pColumns->columnWeight] = CtTreeIter::get_pango_weight_from_is_bold(true);
        (*treeIterUp)[_pColumns->columnWeight] = CtTreeIter::get_pango_weight_from_is_bold(false);
    }
    exit_cell_edit();
    _pListStore->iter_swap(treeIter, treeIterUp);
    _currentRow = rowIdxUp;
    if (not from_move_down) {
        grab_focus();
    }
}

bool CtTableLight::_on_cell_key_press_alt_or_ctrl_enter()
{
    if (not _pEditingCellEntry) {
        return true; /* stop signal */
    }
    int position = _pEditingCellEntry->get_position();
    _pEditingCellEntry->insert_text(CtConst::CHAR_NEWLINE, -1, position);
    _pEditingCellEntry->set_position(position+1);
    return true; /* stop signal */
}

bool CtTableLight::_row_sort(const bool sortAsc)
{
    auto f_need_swap = [sortAsc, this](Gtk::TreeIter& l, Gtk::TreeIter& r)->bool{
        const size_t minCols = get_num_columns();
        for (size_t c = 0; c < minCols; ++c) {
            const int cmpResult = CtStrUtil::natural_compare((*l)[_pColumns->columnsText.at(c)],
                                                             (*r)[_pColumns->columnsText.at(c)]);
            if (0 != cmpResult) {
                return sortAsc ? cmpResult > 0 : cmpResult < 0;
            }
        }
        return false; // no swap needed as equal
    };
    exit_cell_edit();
    const bool retVal = CtMiscUtil::node_siblings_sort(_pListStore,
                                                       _pListStore->children(),
                                                       f_need_swap,
                                                       1u/*start_offset*/);
    return retVal;
}

void CtTableLight::column_add(const size_t afterColIdx)
{
    const size_t currNumCol = get_num_columns();
    CtTableMatrix tableMatrix;
    tableMatrix.reserve(get_num_rows());
    const CtTableLightColumns& cols = get_columns();
    auto f_action = [&](const Gtk::TreeIter& treeIter)->bool{
        Gtk::TreeRow treeRow = *treeIter;
        tableMatrix.push_back(CtTableRow{});
        tableMatrix.back().reserve(currNumCol+1u);
        for (size_t c = 0u; c < currNumCol; ++c) {
            tableMatrix.back().push_back(new Glib::ustring{treeRow.get_value(cols.columnsText.at(c))});
            if (afterColIdx == c) {
                tableMatrix.back().push_back(new Glib::ustring{});
            }
        }
        return false; /* to continue */
    };
    exit_cell_edit();
    _pListStore->foreach_iter(f_action);
    _colWidths.insert(_colWidths.begin()+afterColIdx+1, 0);
    _reset(tableMatrix);
}

void CtTableLight::column_delete(const size_t colIdx)
{
    const size_t currNumCol = get_num_columns();
    if (1u == currNumCol or colIdx >= currNumCol) {
        return;
    }
    CtTableMatrix tableMatrix;
    tableMatrix.reserve(get_num_rows());
    const CtTableLightColumns& cols = get_columns();
    auto f_action = [&](const Gtk::TreeIter& treeIter)->bool{
        Gtk::TreeRow treeRow = *treeIter;
        tableMatrix.push_back(CtTableRow{});
        tableMatrix.back().reserve(currNumCol-1u);
        for (size_t c = 0u; c < currNumCol; ++c) {
            if (colIdx != c) {
                tableMatrix.back().push_back(new Glib::ustring{treeRow.get_value(cols.columnsText.at(c))});
            }
        }
        return false; /* to continue */
    };
    exit_cell_edit();
    _pListStore->foreach_iter(f_action);
    _colWidths.erase(_colWidths.begin()+colIdx);
    _reset(tableMatrix);
    if (_currentColumn == get_num_columns()) {
        --_currentColumn;
    }
    grab_focus();
}

void CtTableLight::column_move_left(const size_t colIdx, const bool from_move_right)
{
    if (0 == colIdx) {
        return;
    }
    const size_t colIdxLeft{colIdx - 1u};
    const CtTableLightColumns& cols = get_columns();
    auto f_action = [&](const Gtk::TreeIter& treeIter)->bool{
        Gtk::TreeRow treeRow = *treeIter;
        const Glib::ustring tmpCell = treeRow[cols.columnsText.at(colIdx)];
        treeRow[cols.columnsText.at(colIdx)] = treeRow.get_value(cols.columnsText.at(colIdxLeft));
        treeRow[cols.columnsText.at(colIdxLeft)] = tmpCell;
        return false; /* to continue */
    };
    exit_cell_edit();
    _pListStore->foreach_iter(f_action);
    _currentColumn = colIdxLeft;
    if (not from_move_right) {
        grab_focus();
    }
}

void CtTableLight::column_move_right(const size_t colIdx)
{
    if (colIdx == get_num_columns()-1) {
        return;
    }
    const size_t colIdxRight = colIdx + 1;
    column_move_left(colIdxRight, true/*from_move_right*/);
    _currentColumn = colIdxRight;
    grab_focus();
}

void CtTableLight::set_col_width_default(const int colWidthDefault)
{
    _colWidthDefault = colWidthDefault;
    bool has_default_widths = vec::exists(_colWidths, 0);
    if (has_default_widths) {
        const size_t numColumns = get_num_columns();
        for (size_t c = 0u; c < numColumns; ++c) {
            if (0u == _colWidths.at(c)) {
                Gtk::TreeViewColumn* pTVColumn = _pManagedTreeView->get_column(c);
                if (pTVColumn) {
                    auto pCellRendererText = static_cast<Gtk::CellRendererText*>(pTVColumn->get_first_cell());
                    pCellRendererText->property_wrap_width() = colWidthDefault;
                    pTVColumn->property_min_width() = colWidthDefault/2;
                }
            }
        }
    }
}

void CtTableLight::set_col_width(const int colWidth, std::optional<size_t> optColIdx/*= std::nullopt*/)
{
    const size_t c = optColIdx.value_or(_currentColumn);
    _colWidths[c] = colWidth;
    Gtk::TreeViewColumn* pTVColumn = _pManagedTreeView->get_column(c);
    if (pTVColumn) {
        auto pCellRendererText = static_cast<Gtk::CellRendererText*>(pTVColumn->get_first_cell());
        pCellRendererText->property_wrap_width() = colWidth;
        pTVColumn->property_min_width() = colWidth/2;
    }
}

std::string CtTableLight::to_csv() const
{
    CtCSV::CtStringTable tbl;
    tbl.reserve(get_num_rows());
    const size_t numColumns = get_num_columns();
    const CtTableLightColumns& cols = get_columns();
    auto f_action = [&](const Gtk::TreeIter& treeIter)->bool{
        Gtk::TreeRow treeRow = *treeIter;
        std::vector<std::string> row;
        row.reserve(numColumns);
        for (size_t c = 0u; c < numColumns; ++c) {
            row.emplace_back(treeRow.get_value(cols.columnsText.at(c)));
        }
        tbl.emplace_back(row);
        return false; /* to continue */
    };
    _pListStore->foreach_iter(f_action);
    return CtCSV::table_to_csv(tbl);
}

void CtTableLight::grab_focus() const
{
    const size_t currRow = current_row();
    const size_t currCol = current_column();
    //spdlog::debug("focus ({},{})", currRow, currCol);
    for (int i = 0; i < 2; ++i) {
        _pCtMainWin->get_text_view().grab_focus();
        while (gtk_events_pending()) gtk_main_iteration();
        _pManagedTreeView->set_cursor(Gtk::TreePath{std::to_string(currRow)},
                                      *_pManagedTreeView->get_column(currCol),
                                      0 != i/*start_editing*/);
    }
}

void CtTableLight::exit_cell_edit() const
{
    _pManagedTreeView->set_cursor(Gtk::TreePath{std::to_string(current_row())});
}

Glib::ustring CtTableLight::get_line_content(const size_t rowIdx, const size_t colIdx, const int match_end_offset) const
{
    Gtk::TreePath treePath{std::to_string(rowIdx)};
    Gtk::TreeIter treeIter = _pListStore->get_iter(treePath);
    if (not treeIter) {
        spdlog::warn("!! {} row {}", __FUNCTION__, rowIdx);
        return "!?";
    }
    Gtk::TreeRow treeRow = *treeIter;
    const CtTableLightColumns& cols = get_columns();
    if (cols.columnsText.size() <= colIdx) {
        spdlog::warn("!! {} col {}", __FUNCTION__, colIdx);
        return "!?";
    }
    Glib::ustring cellText = treeRow[cols.columnsText.at(colIdx)];
    return CtTextIterUtil::get_line_content(cellText, match_end_offset);
}

void CtTableLight::_on_treeview_event_after(GdkEvent* event)
{
    if (event->type == GDK_BUTTON_PRESS and event->button.button == 1) {
        Gtk::TreePath path_at_click;
        int cell_x, cell_y;
        Gtk::TreeViewColumn* pCol_at_click;
        const int event_x = (int)event->button.x;
        const int event_y = (int)event->button.y;
        if (_pManagedTreeView->get_path_at_pos(event_x, event_y, path_at_click, pCol_at_click, cell_x, cell_y)) {
            //spdlog::debug("click x={} y={}", event_x, event_y);
            const size_t numColumns = get_num_columns();
            size_t selCol{0u};
            for (int c = numColumns - 1; c >= 0; --c) {
                Gtk::TreeViewColumn* pColumn = _pManagedTreeView->get_column(c);
                Gdk::Rectangle rect;
                _pManagedTreeView->get_cell_area(path_at_click, *pColumn, rect);
                //spdlog::debug("cell {} x: {}", c, rect.get_x());
                if (event_x >= rect.get_x()) {
                    selCol = c;
                    //spdlog::debug("focus ({},{}) x={} y={}", path_at_click.to_string().raw(), selCol, event_x, event_y);
                    break;
                }
            }
            _currentRow = std::stoi(path_at_click.to_string().raw());
            _currentColumn = selCol;
            grab_focus();
        }
    }
}
