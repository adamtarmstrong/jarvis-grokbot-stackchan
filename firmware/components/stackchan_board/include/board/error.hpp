// SPDX-FileCopyrightText: 2026 Kenta IDA
// SPDX-License-Identifier: BSL-1.0
//
// Local stand-in for groki-bot's board/board.hpp. Only the error codes used by
// the Si12T and PY32 drivers are kept. The original header also pulls in the
// whole board detector, which this firmware does not build.

#pragma once

namespace stackchan::board {

enum class Error {
    ExpanderProbe,
    ExpanderWrite,
    TouchProbe,
    TouchRead,
};

} // namespace stackchan::board
