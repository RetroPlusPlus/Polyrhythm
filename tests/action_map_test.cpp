// Device-free coverage for the action-binding surface (input_actions.h): the ActionMap value, the
// Source vocabulary and its implicit conversions, preset bundles, the pad-button resolution table
// (labeled aliases per family, Sony synonyms), the family-qualified suppression rule, and the
// digital thresholds of analog-backed sources.
#include <gtest/gtest.h>

#include "retropp/input_actions.h"

namespace retropp {
namespace {

// The test game's vocabulary — the map never sees these names, only the ids.
enum class Act : std::uint8_t { Up, Down, Left, Right, Confirm, Cancel, Fire, Move, Throttle };

int countRows(const ActionMap& m, Act a) {
    int n = 0;
    for (const ActionBinding& row : m.rows()) {
        if (row.action == actionId(a)) ++n;
    }
    return n;
}

bool hasRow(const ActionMap& m, Act a, const Source& s) {
    for (const ActionBinding& row : m.rows()) {
        if (row.action == actionId(a) && row.source == s) return true;
    }
    return false;
}

// ── The map value ─────────────────────────────────────────────────────────────────────────────────

TEST(ActionMap, DefaultIsEmpty) {
    ActionMap m;
    EXPECT_TRUE(m.rows().empty());  // no vocabulary of the engine's own — an unmapped engine is silent
}

TEST(ActionMap, DeclarativeRowsFlattenToOneBindingPerSource) {
    const ActionMap m{
        {Act::Fire, {SDL_SCANCODE_X, PadButton::FaceLabelA, MouseButton::Left}},
        {Act::Confirm, {SDL_SCANCODE_RETURN, PadButton::FaceSouth}},
    };
    EXPECT_EQ(m.rows().size(), 5u);
    EXPECT_EQ(countRows(m, Act::Fire), 3);
    EXPECT_EQ(countRows(m, Act::Confirm), 2);
    EXPECT_TRUE(hasRow(m, Act::Fire, Source{SDL_SCANCODE_X}));
    EXPECT_TRUE(hasRow(m, Act::Fire, Source{PadButton::FaceLabelA}));
    EXPECT_TRUE(hasRow(m, Act::Fire, Source{MouseButton::Left}));
}

TEST(ActionMap, BindAppendsARow) {
    ActionMap m;
    m.bind(Act::Fire, SDL_SCANCODE_X);
    m.bind(Act::Fire, PadButton::FaceSouth);  // multi-source = another row, same action
    EXPECT_EQ(countRows(m, Act::Fire), 2);
}

TEST(ActionMap, UnbindRemovesTheExactRowOnly) {
    ActionMap m;
    m.bind(Act::Fire, SDL_SCANCODE_X);
    m.bind(Act::Fire, PadButton::FaceSouth);
    m.bind(Act::Confirm, SDL_SCANCODE_X);  // same key on a different action — must survive
    m.unbind(Act::Fire, SDL_SCANCODE_X);
    EXPECT_FALSE(hasRow(m, Act::Fire, Source{SDL_SCANCODE_X}));
    EXPECT_TRUE(hasRow(m, Act::Fire, Source{PadButton::FaceSouth}));
    EXPECT_TRUE(hasRow(m, Act::Confirm, Source{SDL_SCANCODE_X}));
}

TEST(ActionMap, ClearActionRemovesEveryRowForTheAction) {
    ActionMap m{
        {Act::Fire, {SDL_SCANCODE_X, PadButton::FaceSouth, MouseButton::Left}},
        {Act::Cancel, {SDL_SCANCODE_ESCAPE}},
    };
    m.clearAction(Act::Fire);
    EXPECT_EQ(countRows(m, Act::Fire), 0);
    EXPECT_EQ(countRows(m, Act::Cancel), 1);
}

TEST(ActionMap, AddMergesABundlesRows) {
    ActionMap m{{Act::Confirm, {SDL_SCANCODE_RETURN}}};
    ActionMap bundle;
    bundle.bind(Act::Fire, PadButton::FaceSouth);
    m.add(bundle);
    EXPECT_EQ(m.rows().size(), 2u);
    EXPECT_TRUE(hasRow(m, Act::Fire, Source{PadButton::FaceSouth}));
}

TEST(ActionMap, ValueSemanticsCopyIsIndependent) {
    ActionMap a;
    a.bind(Act::Fire, SDL_SCANCODE_X);
    ActionMap b = a;  // the game hands the platform a COPY; editing its own does not leak through
    b.bind(Act::Fire, PadButton::FaceSouth);
    EXPECT_EQ(countRows(a, Act::Fire), 1);
    EXPECT_EQ(countRows(b, Act::Fire), 2);
}

// ── Source construction ───────────────────────────────────────────────────────────────────────────

TEST(Source, ImplicitConversionTagsTheKind) {
    const Source key = SDL_SCANCODE_E;
    const Source pad = PadButton::ShoulderR;
    const Source mouse = MouseButton::Right;
    const Source stick = PadStick::Left;
    EXPECT_EQ(key.kind, Source::Kind::Key);
    EXPECT_EQ(key.key, SDL_SCANCODE_E);
    EXPECT_EQ(pad.kind, Source::Kind::Pad);
    EXPECT_EQ(pad.pad, PadButton::ShoulderR);
    EXPECT_EQ(mouse.kind, Source::Kind::Mouse);
    EXPECT_EQ(mouse.mouse, MouseButton::Right);
    EXPECT_EQ(stick.kind, Source::Kind::Stick);
    EXPECT_EQ(stick.stick, PadStick::Left);
}

TEST(Source, OnPadQualifiesTheFamily) {
    const Source s = onPad(ControllerType::Nintendo, PadButton::FaceEast);
    ASSERT_TRUE(s.family.has_value());
    EXPECT_EQ(*s.family, ControllerType::Nintendo);
    EXPECT_EQ(s.pad, PadButton::FaceEast);
    EXPECT_FALSE(Source{PadButton::FaceEast}.family.has_value());  // unqualified by default
}

TEST(Source, BuildersComposeThresholdAndComponent) {
    const Source s = asComponent(withThreshold(PadButton::TriggerR, 0.6f), Dir::Up);
    EXPECT_FLOAT_EQ(s.threshold, 0.6f);
    EXPECT_EQ(s.component, Dir::Up);
    EXPECT_EQ(s.pad, PadButton::TriggerR);
}

// ── Sony synonyms: equal-value aliases, no resolution ────────────────────────────────────────────

TEST(PadButtonVocabulary, SonySymbolsAreTheCardinals) {
    EXPECT_EQ(PadButton::FaceCross, PadButton::FaceSouth);
    EXPECT_EQ(PadButton::FaceCircle, PadButton::FaceEast);
    EXPECT_EQ(PadButton::FaceSquare, PadButton::FaceWest);
    EXPECT_EQ(PadButton::FaceTriangle, PadButton::FaceNorth);
}

// ── The resolution table (resolvePadButton) ──────────────────────────────────────────────────────

TEST(PadButtonResolution, PositionalCardinalsAreFamilyIndependent) {
    for (const ControllerType family :
         {ControllerType::Unknown, ControllerType::Xbox, ControllerType::PlayStation,
          ControllerType::Nintendo, ControllerType::Standard}) {
        EXPECT_EQ(resolvePadButton(PadButton::FaceSouth, family), SDL_GAMEPAD_BUTTON_SOUTH);
        EXPECT_EQ(resolvePadButton(PadButton::FaceEast, family), SDL_GAMEPAD_BUTTON_EAST);
        EXPECT_EQ(resolvePadButton(PadButton::FaceWest, family), SDL_GAMEPAD_BUTTON_WEST);
        EXPECT_EQ(resolvePadButton(PadButton::FaceNorth, family), SDL_GAMEPAD_BUTTON_NORTH);
    }
}

TEST(PadButtonResolution, LabelAliasesFollowThePrintedLetters) {
    // Nintendo transposes the printed A/B and X/Y versus the Xbox layout; families without letter
    // labels (PlayStation, Standard, Unknown) resolve to the Xbox-convention position.
    for (const ControllerType xboxLike :
         {ControllerType::Xbox, ControllerType::PlayStation, ControllerType::Standard,
          ControllerType::Unknown}) {
        EXPECT_EQ(resolvePadButton(PadButton::FaceLabelA, xboxLike), SDL_GAMEPAD_BUTTON_SOUTH);
        EXPECT_EQ(resolvePadButton(PadButton::FaceLabelB, xboxLike), SDL_GAMEPAD_BUTTON_EAST);
        EXPECT_EQ(resolvePadButton(PadButton::FaceLabelX, xboxLike), SDL_GAMEPAD_BUTTON_WEST);
        EXPECT_EQ(resolvePadButton(PadButton::FaceLabelY, xboxLike), SDL_GAMEPAD_BUTTON_NORTH);
    }
    EXPECT_EQ(resolvePadButton(PadButton::FaceLabelA, ControllerType::Nintendo),
              SDL_GAMEPAD_BUTTON_EAST);
    EXPECT_EQ(resolvePadButton(PadButton::FaceLabelB, ControllerType::Nintendo),
              SDL_GAMEPAD_BUTTON_SOUTH);
    EXPECT_EQ(resolvePadButton(PadButton::FaceLabelX, ControllerType::Nintendo),
              SDL_GAMEPAD_BUTTON_NORTH);
    EXPECT_EQ(resolvePadButton(PadButton::FaceLabelY, ControllerType::Nintendo),
              SDL_GAMEPAD_BUTTON_WEST);
}

TEST(PadButtonResolution, NeutralPhysicalNamesResolveToTheirSdlButtons) {
    EXPECT_EQ(resolvePadButton(PadButton::DpadUp, ControllerType::Standard),
              SDL_GAMEPAD_BUTTON_DPAD_UP);
    EXPECT_EQ(resolvePadButton(PadButton::ShoulderL, ControllerType::Standard),
              SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    EXPECT_EQ(resolvePadButton(PadButton::ShoulderR, ControllerType::PlayStation),
              SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);  // R1 is the same physical button
    EXPECT_EQ(resolvePadButton(PadButton::StickClickL, ControllerType::Standard),
              SDL_GAMEPAD_BUTTON_LEFT_STICK);
    EXPECT_EQ(resolvePadButton(PadButton::StickClickR, ControllerType::Standard),
              SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    EXPECT_EQ(resolvePadButton(PadButton::Start, ControllerType::Standard),
              SDL_GAMEPAD_BUTTON_START);
    EXPECT_EQ(resolvePadButton(PadButton::Select, ControllerType::Standard),
              SDL_GAMEPAD_BUTTON_BACK);
    EXPECT_EQ(resolvePadButton(PadButton::Guide, ControllerType::Standard),
              SDL_GAMEPAD_BUTTON_GUIDE);
    EXPECT_EQ(resolvePadButton(PadButton::Share, ControllerType::Standard),
              SDL_GAMEPAD_BUTTON_MISC1);  // Share · Create · Capture — one neutral name
    EXPECT_EQ(resolvePadButton(PadButton::Share, ControllerType::PlayStation),
              SDL_GAMEPAD_BUTTON_MISC1);
}

TEST(PadButtonResolution, AnalogBackedEntriesReadAxesNotButtons) {
    for (const PadButton b :
         {PadButton::TriggerL, PadButton::TriggerR, PadButton::LeftStickUp,
          PadButton::LeftStickDown, PadButton::LeftStickLeft, PadButton::LeftStickRight,
          PadButton::RightStickUp, PadButton::RightStickDown, PadButton::RightStickLeft,
          PadButton::RightStickRight}) {
        EXPECT_TRUE(padButtonIsAnalog(b));
        EXPECT_EQ(resolvePadButton(b, ControllerType::Standard), SDL_GAMEPAD_BUTTON_INVALID);
    }
    EXPECT_FALSE(padButtonIsAnalog(PadButton::FaceSouth));
    EXPECT_FALSE(padButtonIsAnalog(PadButton::ShoulderR));
}

// ── Thresholds ────────────────────────────────────────────────────────────────────────────────────

TEST(Thresholds, PerKindDefaultsApplyWhenUnset) {
    EXPECT_FLOAT_EQ(sourceThreshold(Source{PadButton::TriggerL}), kTriggerThreshold);
    EXPECT_FLOAT_EQ(sourceThreshold(Source{PadButton::TriggerR}), kTriggerThreshold);
    EXPECT_FLOAT_EQ(sourceThreshold(Source{PadButton::LeftStickUp}), kStickDirThreshold);
    EXPECT_FLOAT_EQ(sourceThreshold(Source{PadStick::Left}), kStickDirThreshold);
}

TEST(Thresholds, ExplicitOverrideWins) {
    EXPECT_FLOAT_EQ(sourceThreshold(withThreshold(PadButton::TriggerL, 0.75f)), 0.75f);
    EXPECT_FLOAT_EQ(sourceThreshold(withThreshold(PadStick::Right, 0.9f)), 0.9f);
}

// ── The family-qualified suppression rule ─────────────────────────────────────────────────────────

TEST(Suppression, QualifiedMaskCollectsPadAndStickFamiliesPerAction) {
    const ActionMap m{
        {Act::Confirm, {onPad(ControllerType::Nintendo, PadButton::FaceEast),
                        onPad(ControllerType::PlayStation, PadButton::FaceCross),
                        PadButton::FaceSouth, SDL_SCANCODE_RETURN}},
        {Act::Cancel, {PadButton::FaceEast}},
    };
    const std::uint8_t confirmMask = qualifiedFamilyMask(m.rows(), actionId(Act::Confirm));
    EXPECT_NE(confirmMask & (1u << static_cast<unsigned>(ControllerType::Nintendo)), 0);
    EXPECT_NE(confirmMask & (1u << static_cast<unsigned>(ControllerType::PlayStation)), 0);
    EXPECT_EQ(confirmMask & (1u << static_cast<unsigned>(ControllerType::Xbox)), 0);
    EXPECT_EQ(qualifiedFamilyMask(m.rows(), actionId(Act::Cancel)), 0);  // no qualified rows
}

TEST(Suppression, QualifiedRowAppliesOnlyOnItsFamily) {
    const Source nintendoRow = onPad(ControllerType::Nintendo, PadButton::FaceEast);
    const std::uint8_t mask = 1u << static_cast<unsigned>(ControllerType::Nintendo);
    EXPECT_TRUE(padRowAppliesTo(nintendoRow, ControllerType::Nintendo, mask));
    EXPECT_FALSE(padRowAppliesTo(nintendoRow, ControllerType::Xbox, mask));
    EXPECT_FALSE(padRowAppliesTo(nintendoRow, ControllerType::PlayStation, mask));
}

TEST(Suppression, UnqualifiedRowIsSuppressedOnAQualifiedFamily) {
    const Source generic = PadButton::FaceSouth;
    const std::uint8_t nintendoQualified = 1u << static_cast<unsigned>(ControllerType::Nintendo);
    // On a Switch pad the action has explicit rows → the generic row is suppressed there…
    EXPECT_FALSE(padRowAppliesTo(generic, ControllerType::Nintendo, nintendoQualified));
    // …and still serves every family without qualified rows.
    EXPECT_TRUE(padRowAppliesTo(generic, ControllerType::Xbox, nintendoQualified));
    EXPECT_TRUE(padRowAppliesTo(generic, ControllerType::PlayStation, nintendoQualified));
    EXPECT_TRUE(padRowAppliesTo(generic, ControllerType::Standard, 0));
}

TEST(Suppression, ConfirmCancelSwitchSwapIsTotal) {
    // The canonical scenario: Confirm = printed-A, Cancel = printed-B, explicit on Nintendo. On a
    // Switch pad each physical button must drive exactly ONE action — the generic rows suppress.
    const ActionMap m{
        {Act::Confirm, {PadButton::FaceSouth, onPad(ControllerType::Nintendo, PadButton::FaceEast)}},
        {Act::Cancel, {PadButton::FaceEast, onPad(ControllerType::Nintendo, PadButton::FaceSouth)}},
    };
    const std::uint8_t confirmMask = qualifiedFamilyMask(m.rows(), actionId(Act::Confirm));
    const std::uint8_t cancelMask  = qualifiedFamilyMask(m.rows(), actionId(Act::Cancel));

    // On Nintendo: east drives Confirm only; south drives Cancel only. No double-fire.
    EXPECT_TRUE(padRowAppliesTo(onPad(ControllerType::Nintendo, PadButton::FaceEast),
                                ControllerType::Nintendo, confirmMask));
    EXPECT_FALSE(padRowAppliesTo(Source{PadButton::FaceSouth}, ControllerType::Nintendo, confirmMask));
    EXPECT_TRUE(padRowAppliesTo(onPad(ControllerType::Nintendo, PadButton::FaceSouth),
                                ControllerType::Nintendo, cancelMask));
    EXPECT_FALSE(padRowAppliesTo(Source{PadButton::FaceEast}, ControllerType::Nintendo, cancelMask));

    // On Xbox: the generic rows apply, the Nintendo rows do not.
    EXPECT_TRUE(padRowAppliesTo(Source{PadButton::FaceSouth}, ControllerType::Xbox, confirmMask));
    EXPECT_FALSE(padRowAppliesTo(onPad(ControllerType::Nintendo, PadButton::FaceEast),
                                 ControllerType::Xbox, confirmMask));
}

TEST(Suppression, FaceLabelAIsSugarForTheQualifiedPair) {
    // FaceLabelA resolves exactly like the explicit {onPad(Nintendo, FaceEast), FaceSouth} pair.
    for (const ControllerType family :
         {ControllerType::Xbox, ControllerType::PlayStation, ControllerType::Standard,
          ControllerType::Nintendo}) {
        const SDL_GamepadButton viaAlias = resolvePadButton(PadButton::FaceLabelA, family);
        const SDL_GamepadButton viaPair  = (family == ControllerType::Nintendo)
                                               ? resolvePadButton(PadButton::FaceEast, family)
                                               : resolvePadButton(PadButton::FaceSouth, family);
        EXPECT_EQ(viaAlias, viaPair);
    }
}

// ── Presets ───────────────────────────────────────────────────────────────────────────────────────

TEST(Presets, DirectionalExpandsToArrowsWasdAndDpad) {
    const ActionMap m = presets::directional(Act::Up, Act::Down, Act::Left, Act::Right);
    EXPECT_EQ(m.rows().size(), 12u);  // 4 directions × (arrow + WASD key + d-pad)
    EXPECT_TRUE(hasRow(m, Act::Up, Source{SDL_SCANCODE_UP}));
    EXPECT_TRUE(hasRow(m, Act::Up, Source{SDL_SCANCODE_W}));
    EXPECT_TRUE(hasRow(m, Act::Up, Source{PadButton::DpadUp}));
    EXPECT_TRUE(hasRow(m, Act::Down, Source{SDL_SCANCODE_S}));
    EXPECT_TRUE(hasRow(m, Act::Left, Source{SDL_SCANCODE_A}));
    EXPECT_TRUE(hasRow(m, Act::Right, Source{SDL_SCANCODE_D}));
    EXPECT_TRUE(hasRow(m, Act::Right, Source{PadButton::DpadRight}));
}

TEST(Presets, DirectionalVectorBindsStickPlusComponentTaggedKeys) {
    const ActionMap m = presets::directionalVector(Act::Move);
    EXPECT_EQ(m.rows().size(), 13u);  // the stick + 4 directions × (arrow + WASD + d-pad)
    EXPECT_TRUE(hasRow(m, Act::Move, Source{PadStick::Left}));
    EXPECT_TRUE(hasRow(m, Act::Move, asComponent(SDL_SCANCODE_W, Dir::Up)));
    EXPECT_TRUE(hasRow(m, Act::Move, asComponent(PadButton::DpadDown, Dir::Down)));
    EXPECT_TRUE(hasRow(m, Act::Move, asComponent(SDL_SCANCODE_A, Dir::Left)));
    EXPECT_TRUE(hasRow(m, Act::Move, asComponent(SDL_SCANCODE_RIGHT, Dir::Right)));
}

TEST(Presets, BundleMergesIntoAGameMapWithoutDisturbingIt) {
    ActionMap m{{Act::Fire, {SDL_SCANCODE_X}}};
    m.add(presets::directional(Act::Up, Act::Down, Act::Left, Act::Right));
    EXPECT_EQ(m.rows().size(), 13u);
    EXPECT_TRUE(hasRow(m, Act::Fire, Source{SDL_SCANCODE_X}));
    EXPECT_TRUE(hasRow(m, Act::Up, Source{SDL_SCANCODE_W}));
}

// ── controllerTypeFrom (relocated with the SDL-coupled surface) ───────────────────────────────────

TEST(ControllerFamily, SdlTypesCollapseToEngineFamilies) {
    EXPECT_EQ(controllerTypeFrom(SDL_GAMEPAD_TYPE_XBOXONE), ControllerType::Xbox);
    EXPECT_EQ(controllerTypeFrom(SDL_GAMEPAD_TYPE_XBOX360), ControllerType::Xbox);
    EXPECT_EQ(controllerTypeFrom(SDL_GAMEPAD_TYPE_PS5), ControllerType::PlayStation);
    EXPECT_EQ(controllerTypeFrom(SDL_GAMEPAD_TYPE_PS4), ControllerType::PlayStation);
    EXPECT_EQ(controllerTypeFrom(SDL_GAMEPAD_TYPE_PS3), ControllerType::PlayStation);
    EXPECT_EQ(controllerTypeFrom(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO), ControllerType::Nintendo);
    EXPECT_EQ(controllerTypeFrom(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR),
              ControllerType::Nintendo);
    EXPECT_EQ(controllerTypeFrom(SDL_GAMEPAD_TYPE_STANDARD), ControllerType::Standard);
    EXPECT_EQ(controllerTypeFrom(SDL_GAMEPAD_TYPE_UNKNOWN), ControllerType::Unknown);
}

}  // namespace
}  // namespace retropp
