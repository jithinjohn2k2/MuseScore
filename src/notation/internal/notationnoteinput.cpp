/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore BVBA and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "notationnoteinput.h"

#include "libmscore/masterscore.h"
#include "libmscore/input.h"
#include "libmscore/staff.h"
#include "libmscore/note.h"
#include "libmscore/chord.h"
#include "libmscore/slur.h"
#include "libmscore/articulation.h"
#include "libmscore/system.h"
#include "libmscore/stafftype.h"

#include "scorecallbacks.h"

#include "log.h"

using namespace mu::notation;
using namespace mu::async;

NotationNoteInput::NotationNoteInput(const IGetScore* getScore, INotationInteraction* interaction, INotationUndoStackPtr undoStack)
    : m_getScore(getScore), m_interaction(interaction), m_undoStack(undoStack)
{
    m_scoreCallbacks = new ScoreCallbacks();
    m_scoreCallbacks->setNotationInteraction(interaction);

    m_interaction->selectionChanged().onNotify(this, [this]() {
        if (!isNoteInputMode()) {
            updateInputState();
        }
    });
}

NotationNoteInput::~NotationNoteInput()
{
    delete m_scoreCallbacks;
}

bool NotationNoteInput::isNoteInputMode() const
{
    return score()->inputState().noteEntryMode();
}

NoteInputState NotationNoteInput::state() const
{
    const mu::engraving::InputState& inputState = score()->inputState();

    NoteInputState noteInputState;
    noteInputState.method = inputState.noteEntryMethod();
    noteInputState.duration = inputState.duration();
    noteInputState.accidentalType = inputState.accidentalType();
    noteInputState.articulationIds = articulationIds();
    noteInputState.withSlur = inputState.slur() != nullptr;
    noteInputState.currentVoiceIndex = inputState.voice();
    noteInputState.currentTrack = inputState.track();
    noteInputState.drumset = inputState.drumset();
    noteInputState.isRest = inputState.rest();
    noteInputState.staffGroup = inputState.staffGroup();

    return noteInputState;
}

//! NOTE Coped from `void ScoreView::startNoteEntry()`
void NotationNoteInput::startNoteInput()
{
    TRACEFUNC;

    if (isNoteInputMode()) {
        return;
    }

    mu::engraving::EngravingItem* el = resolveNoteInputStartPosition();
    if (!el) {
        return;
    }

    m_interaction->select({ el }, SelectType::SINGLE, 0);

    mu::engraving::InputState& is = score()->inputState();

    // Not strictly necessary, just for safety
    if (is.noteEntryMethod() == mu::engraving::NoteEntryMethod::UNKNOWN) {
        is.setNoteEntryMethod(mu::engraving::NoteEntryMethod::STEPTIME);
    }

    Duration d(is.duration());
    if (!d.isValid() || d.isZero() || d.type() == DurationType::V_MEASURE) {
        is.setDuration(Duration(DurationType::V_QUARTER));
    }
    is.setAccidentalType(mu::engraving::AccidentalType::NONE);

    is.setRest(false);
    is.setNoteEntryMode(true);

    //! TODO Find out why.
    score()->setUpdateAll();
    score()->update();
    //! ---

    const Staff* staff = score()->staff(is.track() / mu::engraving::VOICES);
    switch (staff->staffType(is.tick())->group()) {
    case mu::engraving::StaffGroup::STANDARD:
        break;
    case mu::engraving::StaffGroup::TAB: {
        int strg = 0;                           // assume topmost string as current string
        // if entering note entry with a note selected and the note has a string
        // set InputState::_string to note physical string
        if (el->type() == ElementType::NOTE) {
            strg = (static_cast<mu::engraving::Note*>(el))->string();
        }
        is.setString(strg);
        break;
    }
    case mu::engraving::StaffGroup::PERCUSSION:
        break;
    }

    notifyAboutStateChanged();

    m_interaction->showItem(el);
}

mu::engraving::EngravingItem* NotationNoteInput::resolveNoteInputStartPosition() const
{
    EngravingItem* el = score()->selection().element();
    if (!el) {
        el = score()->selection().firstChordRest();
    }

    const mu::engraving::InputState& is = score()->inputState();

    if (!el) {
        if (const mu::engraving::Segment* segment = is.lastSegment()) {
            el = segment->element(is.track());
        }
    }

    if (el == nullptr
        || (el->type() != ElementType::CHORD && el->type() != ElementType::REST && el->type() != ElementType::NOTE)) {
        // if no note/rest is selected, start with voice 0
        engraving::track_idx_t track = is.track() == mu::nidx ? 0 : (is.track() / mu::engraving::VOICES) * mu::engraving::VOICES;
        // try to find an appropriate measure to start in
        Fraction tick = el ? el->tick() : Fraction(0, 1);
        el = score()->searchNote(tick, track);
        if (!el) {
            el = score()->searchNote(Fraction(0, 1), track);
        }
    }

    if (!el) {
        return nullptr;
    }

    if (el->type() == ElementType::CHORD) {
        mu::engraving::Chord* c = static_cast<mu::engraving::Chord*>(el);
        mu::engraving::Note* note = c->selectedNote();
        if (note == 0) {
            note = c->upNote();
        }
        el = note;
    }

    return el;
}

void NotationNoteInput::endNoteInput()
{
    TRACEFUNC;

    if (!isNoteInputMode()) {
        return;
    }

    mu::engraving::InputState& is = score()->inputState();
    is.setNoteEntryMode(false);

    if (is.slur()) {
        const std::vector<mu::engraving::SpannerSegment*>& el = is.slur()->spannerSegments();
        if (!el.empty()) {
            el.front()->setSelected(false);
        }
        is.setSlur(0);
    }

    updateInputState();
}

void NotationNoteInput::toggleNoteInputMethod(NoteInputMethod method)
{
    TRACEFUNC;

    score()->inputState().setNoteEntryMethod(method);

    notifyAboutStateChanged();
}

void NotationNoteInput::addNote(NoteName noteName, NoteAddingMode addingMode)
{
    TRACEFUNC;

    mu::engraving::EditData editData(m_scoreCallbacks);

    startEdit();
    int inote = static_cast<int>(noteName);
    bool addToUpOnCurrentChord = addingMode == NoteAddingMode::CurrentChord;
    bool insertNewChord = addingMode == NoteAddingMode::InsertChord;
    score()->cmdAddPitch(editData, inote, addToUpOnCurrentChord, insertNewChord);
    apply();

    notifyNoteAddedChanged();
    notifyAboutStateChanged();
}

void NotationNoteInput::padNote(const Pad& pad)
{
    TRACEFUNC;

    mu::engraving::EditData editData(m_scoreCallbacks);

    startEdit();
    score()->padToggle(pad, editData);
    apply();

    notifyAboutStateChanged();
}

void NotationNoteInput::putNote(const PointF& pos, bool replace, bool insert)
{
    TRACEFUNC;

    startEdit();
    score()->putNote(pos, replace, insert);
    apply();

    notifyNoteAddedChanged();
    notifyAboutStateChanged();
}

void NotationNoteInput::removeNote(const PointF& pos)
{
    TRACEFUNC;

    mu::engraving::InputState& inputState = score()->inputState();
    bool restMode = inputState.rest();

    startEdit();
    inputState.setRest(!restMode);
    score()->putNote(pos, false, false);
    inputState.setRest(restMode);
    apply();

    notifyAboutStateChanged();
}

void NotationNoteInput::setAccidental(AccidentalType accidentalType)
{
    TRACEFUNC;

    mu::engraving::EditData editData(m_scoreCallbacks);

    score()->toggleAccidental(accidentalType, editData);

    notifyAboutStateChanged();
}

void NotationNoteInput::setArticulation(SymbolId articulationSymbolId)
{
    TRACEFUNC;

    mu::engraving::InputState& inputState = score()->inputState();

    std::set<SymbolId> articulations = mu::engraving::updateArticulations(
        inputState.articulationIds(), articulationSymbolId, mu::engraving::ArticulationsUpdateMode::Remove);
    inputState.setArticulationIds(articulations);

    notifyAboutStateChanged();
}

void NotationNoteInput::setDrumNote(int note)
{
    TRACEFUNC;

    score()->inputState().setDrumNote(note);
    notifyAboutStateChanged();
}

void NotationNoteInput::setCurrentVoice(voice_idx_t voiceIndex)
{
    TRACEFUNC;

    if (!isVoiceIndexValid(voiceIndex)) {
        return;
    }

    mu::engraving::InputState& inputState = score()->inputState();
    inputState.setVoice(voiceIndex);

    if (inputState.segment()) {
        mu::engraving::Segment* segment = inputState.segment()->measure()->first(mu::engraving::SegmentType::ChordRest);
        inputState.setSegment(segment);
    }

    notifyAboutStateChanged();
}

void NotationNoteInput::setCurrentTrack(track_idx_t trackIndex)
{
    TRACEFUNC;

    score()->inputState().setTrack(trackIndex);
    notifyAboutStateChanged();
}

void NotationNoteInput::resetInputPosition()
{
    mu::engraving::InputState& inputState = score()->inputState();

    inputState.setTrack(mu::nidx);
    inputState.setString(-1);
    inputState.setSegment(nullptr);

    notifyAboutStateChanged();
}

void NotationNoteInput::addTuplet(const TupletOptions& options)
{
    TRACEFUNC;

    const mu::engraving::InputState& inputState = score()->inputState();

    startEdit();
    score()->expandVoice();
    mu::engraving::ChordRest* chordRest = inputState.cr();
    if (chordRest) {
        score()->changeCRlen(chordRest, inputState.duration());
        score()->addTuplet(chordRest, options.ratio, options.numberType, options.bracketType);
    }
    apply();

    notifyAboutStateChanged();
}

mu::RectF NotationNoteInput::cursorRect() const
{
    TRACEFUNC;

    if (!isNoteInputMode()) {
        return {};
    }

    const mu::engraving::InputState& inputState = score()->inputState();
    const mu::engraving::Segment* segment = inputState.segment();
    if (!segment) {
        return {};
    }

    mu::engraving::System* system = segment->measure()->system();
    if (!system) {
        return {};
    }

    mu::engraving::track_idx_t track = inputState.track() == mu::nidx ? 0 : inputState.track();
    mu::engraving::staff_idx_t staffIdx = track / mu::engraving::VOICES;

    const Staff* staff = score()->staff(staffIdx);
    if (!staff) {
        return {};
    }

    constexpr int sideMargin = 4;
    constexpr int skylineMargin = 20;

    RectF segmentContentRect = segment->contentRect();
    double x = segmentContentRect.translated(segment->pagePos()).x() - sideMargin;
    double y = system->staffYpage(staffIdx) + system->page()->pos().y();
    double w = segmentContentRect.width() + 2 * sideMargin;
    double h = 0.0;

    const mu::engraving::StaffType* staffType = staff->staffType(inputState.tick());
    double spatium = score()->spatium();
    double lineDist = staffType->lineDistance().val() * spatium;
    int lines = staffType->lines();
    int inputStateStringsCount = inputState.string();

    int instrumentStringsCount = static_cast<int>(staff->part()->instrument()->stringData()->strings());
    if (staff->isTabStaff(inputState.tick()) && inputStateStringsCount >= 0 && inputStateStringsCount <= instrumentStringsCount) {
        h = lineDist;
        y += staffType->physStringToYOffset(inputStateStringsCount) * spatium;
        y -= (staffType->onLines() ? lineDist * 0.5 : lineDist);
    } else {
        h = (lines - 1) * lineDist + 2 * skylineMargin;
        y -= skylineMargin;
    }

    RectF result = RectF(x, y, w, h);

    if (configuration()->canvasOrientation().val == framework::Orientation::Horizontal) {
        result.translate(system->page()->pos());
    }

    return result;
}

void NotationNoteInput::addSlur(mu::engraving::Slur* slur)
{
    TRACEFUNC;

    mu::engraving::InputState& inputState = score()->inputState();
    inputState.setSlur(slur);

    if (slur) {
        std::vector<mu::engraving::SpannerSegment*> slurSpannerSegments = slur->spannerSegments();
        if (!slurSpannerSegments.empty()) {
            slurSpannerSegments.front()->setSelected(true);
        }
    }

    notifyAboutStateChanged();
}

void NotationNoteInput::resetSlur()
{
    TRACEFUNC;

    mu::engraving::InputState& inputState = score()->inputState();
    mu::engraving::Slur* slur = inputState.slur();
    if (!slur) {
        return;
    }

    score()->deselect(slur);

    addSlur(nullptr);
}

void NotationNoteInput::addTie()
{
    TRACEFUNC;

    startEdit();
    score()->cmdAddTie();
    apply();

    notifyAboutStateChanged();
}

Notification NotationNoteInput::noteAdded() const
{
    return m_noteAdded;
}

Notification NotationNoteInput::stateChanged() const
{
    return m_stateChanged;
}

mu::engraving::Score* NotationNoteInput::score() const
{
    return m_getScore->score();
}

void NotationNoteInput::startEdit()
{
    m_undoStack->prepareChanges();
}

void NotationNoteInput::apply()
{
    m_undoStack->commitChanges();

    if (mu::engraving::ChordRest* chordRest = score()->inputState().cr()) {
        m_interaction->showItem(chordRest);
    }
}

void NotationNoteInput::updateInputState()
{
    TRACEFUNC;

    score()->inputState().update(score()->selection());

    notifyAboutStateChanged();
}

void NotationNoteInput::notifyAboutStateChanged()
{
    m_stateChanged.notify();
}

void NotationNoteInput::notifyNoteAddedChanged()
{
    m_noteAdded.notify();
}

std::set<SymbolId> NotationNoteInput::articulationIds() const
{
    const mu::engraving::InputState& inputState = score()->inputState();
    return mu::engraving::splitArticulations(inputState.articulationIds());
}

void NotationNoteInput::doubleNoteInputDuration()
{
    TRACEFUNC;

    mu::engraving::EditData editData(m_scoreCallbacks);

    startEdit();
    score()->cmdPadNoteIncreaseTAB(editData);
    apply();

    notifyAboutStateChanged();
}

void NotationNoteInput::halveNoteInputDuration()
{
    TRACEFUNC;

    mu::engraving::EditData editData(m_scoreCallbacks);

    startEdit();
    score()->cmdPadNoteDecreaseTAB(editData);
    apply();

    notifyAboutStateChanged();
}
