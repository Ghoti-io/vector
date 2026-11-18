/**
 * @file
 *
 * The units of a length that is an attribute (not a property, whose computed value the style layer has already put in pixels): the
 * font-relative ones (`em`, `ex`, `ch`, `rem`) and the window's (`vw`, `vh`, `vmin`, `vmax`) are lowered everywhere a length attribute is
 * read, and each gives the list the same length in pixels gives.  The em of a gradient, a pattern, a mask, a filter or a marker is that of
 * the element that has the attribute, not of the element that uses it.
 */

#include "../lower_helpers.h"

using namespace gvec_test;

namespace {

/** A 200 by 100 document whose root font size is 20. */
std::string Doc(const std::string & body, const std::string & extra = "") {
  return Svg("width=\"200\" height=\"100\" font-size=\"20\" " + extra, body);
}

/** Whether the two documents lower with nothing refused and to the same list. */
void ExpectSame(const std::string & with_units, const std::string & in_pixels, const char * what, double viewport_width = 0, double viewport_height = 0) {
  SCOPED_TRACE(what);
  Lowered a, b;
  LowerSvg(with_units, a, viewport_width, viewport_height);
  LowerSvg(in_pixels, b, viewport_width, viewport_height);
  EXPECT_EQ(a.Refusals(), (size_t) 0) << "the units are lowered";
  EXPECT_EQ(b.Refusals(), (size_t) 0);
  ASSERT_NE(a.list, nullptr);
  ASSERT_NE(b.list, nullptr);
  EXPECT_GE(a.Items(), (size_t) 1);
  EXPECT_EQ(gshscene_list_hash(a.list), gshscene_list_hash(b.list)) << "the same list as the same lengths in pixels";
}

const char * kStroke = " stroke=\"black\" fill=\"none\"";

}  // namespace

TEST(LowerUnits, TheEmOfALineIsTheFontSizeOfTheLineAndAnExIsHalfOfIt) {
  ExpectSame(Doc(std::string("<line x1=\"1em\" y1=\"0.5em\" x2=\"5em\" y2=\"2.5em\"") + kStroke + "/>"),
      Doc(std::string("<line x1=\"20\" y1=\"10\" x2=\"100\" y2=\"50\"") + kStroke + "/>"), "em");
  ExpectSame(Doc(std::string("<line x1=\"2ex\" y1=\"0\" x2=\"10ex\" y2=\"5ex\"") + kStroke + "/>"),
      Doc(std::string("<line x1=\"20\" y1=\"0\" x2=\"100\" y2=\"50\"") + kStroke + "/>"), "ex");
  ExpectSame(Doc(std::string("<g font-size=\"10\"><line x1=\"0\" y1=\"0\" x2=\"5em\" y2=\"3em\"") + kStroke + "/></g>"),
      Doc(std::string("<g font-size=\"10\"><line x1=\"0\" y1=\"0\" x2=\"50\" y2=\"30\"") + kStroke + "/></g>"), "the font size of the line's own cascade");
}

TEST(LowerUnits, TheRemIsTheRootsFontSizeWhateverTheElementsOwn) {
  ExpectSame(Doc(std::string("<g font-size=\"10\"><line x1=\"0\" y1=\"0\" x2=\"5rem\" y2=\"2rem\"") + kStroke + "/></g>"),
      Doc(std::string("<g font-size=\"10\"><line x1=\"0\" y1=\"0\" x2=\"100\" y2=\"40\"") + kStroke + "/></g>"), "rem");
}

TEST(LowerUnits, TheWindowsUnitsAreAHundredthOfTheViewportOfTheLoadOptions) {
  ExpectSame(Doc(std::string("<line x1=\"0\" y1=\"0\" x2=\"50vw\" y2=\"50vh\"") + kStroke + "/>"),
      Doc(std::string("<line x1=\"0\" y1=\"0\" x2=\"100\" y2=\"50\"") + kStroke + "/>"), "vw and vh", 200, 100);
  ExpectSame(Doc(std::string("<line x1=\"0\" y1=\"0\" x2=\"50vmin\" y2=\"50vmax\"") + kStroke + "/>"),
      Doc(std::string("<line x1=\"0\" y1=\"0\" x2=\"50\" y2=\"100\"") + kStroke + "/>"), "vmin and vmax", 200, 100);
}

TEST(LowerUnits, ALinearGradientsCoordinatesTakeTheEmOfTheGradientThatHasThem) {
  const std::string shape = "<rect width=\"200\" height=\"100\" fill=\"url(#g)\"/>";
  ExpectSame(Doc("<defs><linearGradient id=\"g\" gradientUnits=\"userSpaceOnUse\" x1=\"1em\" y1=\"0\" x2=\"9em\" y2=\"2em\">"
                 "<stop offset=\"0\" stop-color=\"red\"/><stop offset=\"1\" stop-color=\"blue\"/></linearGradient></defs>" + shape),
      Doc("<defs><linearGradient id=\"g\" gradientUnits=\"userSpaceOnUse\" x1=\"20\" y1=\"0\" x2=\"180\" y2=\"40\">"
          "<stop offset=\"0\" stop-color=\"red\"/><stop offset=\"1\" stop-color=\"blue\"/></linearGradient></defs>" + shape), "inherited from the root");
  ExpectSame(Doc("<defs><linearGradient id=\"g\" gradientUnits=\"userSpaceOnUse\" font-size=\"10\" x1=\"1em\" y1=\"0\" x2=\"9em\" y2=\"2em\">"
                 "<stop offset=\"0\" stop-color=\"red\"/><stop offset=\"1\" stop-color=\"blue\"/></linearGradient></defs>" + shape),
      Doc("<defs><linearGradient id=\"g\" gradientUnits=\"userSpaceOnUse\" x1=\"10\" y1=\"0\" x2=\"90\" y2=\"20\">"
          "<stop offset=\"0\" stop-color=\"red\"/><stop offset=\"1\" stop-color=\"blue\"/></linearGradient></defs>" + shape),
      "the gradient's own font size, not the shape's");
}

TEST(LowerUnits, AnEmOfAnAttributeTakenFromAnotherGradientOrPatternIsThatElementsEm) {
  const std::string stops = "<stop offset=\"0\" stop-color=\"red\"/><stop offset=\"1\" stop-color=\"blue\"/>";
  const std::string shape = "<rect width=\"200\" height=\"100\" fill=\"url(#g)\"/>";
  // `g` has no x2 of its own: it takes the one of `b` through its href, and with it the em of `b`.
  ExpectSame(Doc("<defs><linearGradient id=\"b\" gradientUnits=\"userSpaceOnUse\" font-size=\"10\" x2=\"5em\">" + stops +
                 "</linearGradient><linearGradient id=\"g\" href=\"#b\" font-size=\"30\"/></defs>" + shape),
      Doc("<defs><linearGradient id=\"b\" gradientUnits=\"userSpaceOnUse\" font-size=\"10\" x2=\"50\">" + stops +
          "</linearGradient><linearGradient id=\"g\" href=\"#b\" font-size=\"30\"/></defs>" + shape), "a gradient");
  const std::string body = "<rect width=\"10\" height=\"10\" fill=\"red\"/>";
  const std::string pshape = "<rect width=\"200\" height=\"100\" fill=\"url(#p)\"/>";
  ExpectSame(Doc("<defs><pattern id=\"b\" patternUnits=\"userSpaceOnUse\" font-size=\"10\" width=\"3em\" height=\"3em\">" + body +
                 "</pattern><pattern id=\"p\" href=\"#b\" font-size=\"30\"/></defs>" + pshape),
      Doc("<defs><pattern id=\"b\" patternUnits=\"userSpaceOnUse\" font-size=\"10\" width=\"30\" height=\"30\">" + body +
          "</pattern><pattern id=\"p\" href=\"#b\" font-size=\"30\"/></defs>" + pshape), "a pattern");
}

TEST(LowerUnits, ARadialGradientsCentreRadiusAndFocusTakeTheirUnits) {
  const std::string shape = "<rect width=\"200\" height=\"100\" fill=\"url(#g)\"/>";
  const std::string stops = "<stop offset=\"0\" stop-color=\"red\"/><stop offset=\"1\" stop-color=\"blue\"/>";
  ExpectSame(Doc("<defs><radialGradient id=\"g\" gradientUnits=\"userSpaceOnUse\" cx=\"5em\" cy=\"2em\" r=\"4em\" fx=\"4em\" fy=\"1.5em\" fr=\"0.5em\">" + stops +
                 "</radialGradient></defs>" + shape),
      Doc("<defs><radialGradient id=\"g\" gradientUnits=\"userSpaceOnUse\" cx=\"100\" cy=\"40\" r=\"80\" fx=\"80\" fy=\"30\" fr=\"10\">" + stops +
          "</radialGradient></defs>" + shape), "em");
}

TEST(LowerUnits, APatternsCellTakesTheEm) {
  const std::string body = "<rect width=\"10\" height=\"10\" fill=\"red\"/>";
  const std::string shape = "<rect width=\"200\" height=\"100\" fill=\"url(#p)\"/>";
  ExpectSame(Doc("<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" x=\"0.5em\" y=\"0\" width=\"2em\" height=\"1.5em\">" + body + "</pattern></defs>" + shape),
      Doc("<defs><pattern id=\"p\" patternUnits=\"userSpaceOnUse\" x=\"10\" y=\"0\" width=\"40\" height=\"30\">" + body + "</pattern></defs>" + shape), "em");
}

TEST(LowerUnits, AMasksRegionTakesTheEm) {
  const std::string shape = "<rect width=\"200\" height=\"100\" fill=\"blue\" mask=\"url(#m)\"/>";
  const std::string body = "<rect width=\"200\" height=\"100\" fill=\"white\"/>";
  ExpectSame(Doc("<defs><mask id=\"m\" maskUnits=\"userSpaceOnUse\" x=\"1em\" y=\"0.5em\" width=\"5em\" height=\"3em\">" + body + "</mask></defs>" + shape),
      Doc("<defs><mask id=\"m\" maskUnits=\"userSpaceOnUse\" x=\"20\" y=\"10\" width=\"100\" height=\"60\">" + body + "</mask></defs>" + shape), "em");
}

TEST(LowerUnits, AMasksEmIsTheMasksOwnFontSizeNotTheRoots) {
  const std::string shape = "<rect width=\"200\" height=\"100\" fill=\"blue\" mask=\"url(#m)\"/>";
  const std::string body = "<rect width=\"200\" height=\"100\" fill=\"white\"/>";
  ExpectSame(Doc("<defs><mask id=\"m\" maskUnits=\"userSpaceOnUse\" font-size=\"10\" x=\"1em\" y=\"1em\" width=\"5em\" height=\"3em\">" + body + "</mask></defs>" + shape),
      Doc("<defs><mask id=\"m\" maskUnits=\"userSpaceOnUse\" font-size=\"10\" x=\"10\" y=\"10\" width=\"50\" height=\"30\">" + body + "</mask></defs>" + shape), "em of the mask");
}

TEST(LowerUnits, AFiltersRegionAndAPrimitivesSubregionTakeTheEm) {
  const std::string shape = "<rect x=\"20\" y=\"10\" width=\"100\" height=\"60\" fill=\"blue\" filter=\"url(#f)\"/>";
  ExpectSame(Doc("<defs><filter id=\"f\" filterUnits=\"userSpaceOnUse\" x=\"0.5em\" y=\"0\" width=\"8em\" height=\"4em\">"
                 "<feFlood flood-color=\"red\" x=\"1em\" y=\"1em\" width=\"3em\" height=\"2em\"/></filter></defs>" + shape),
      Doc("<defs><filter id=\"f\" filterUnits=\"userSpaceOnUse\" x=\"10\" y=\"0\" width=\"160\" height=\"80\">"
          "<feFlood flood-color=\"red\" x=\"20\" y=\"20\" width=\"60\" height=\"40\"/></filter></defs>" + shape), "em");
}

TEST(LowerUnits, AMarkersSizeAndReferencePointTakeTheEm) {
  const std::string path = "<path d=\"M10 10L30 10L30 30\" fill=\"none\" stroke=\"none\" marker-start=\"url(#m)\" marker-end=\"url(#m)\"/>";
  const std::string body = "<path d=\"M0 0H4V4H0Z\"/>";
  ExpectSame(Doc("<defs><marker id=\"m\" markerUnits=\"userSpaceOnUse\" markerWidth=\"0.2em\" markerHeight=\"0.3em\" refX=\"0.1em\" refY=\"0.05em\">" + body +
                 "</marker></defs>" + path),
      Doc("<defs><marker id=\"m\" markerUnits=\"userSpaceOnUse\" markerWidth=\"4\" markerHeight=\"6\" refX=\"2\" refY=\"1\">" + body +
          "</marker></defs>" + path), "em");
}

TEST(LowerUnits, ASymbolsReferencePointTakesTheEm) {
  const std::string use = "<use href=\"#s\" x=\"20\" y=\"20\" width=\"10\" height=\"10\"/>";
  const std::string body = "<rect width=\"10\" height=\"10\"/>";
  ExpectSame(Doc("<defs><symbol id=\"s\" viewBox=\"0 0 10 10\" refX=\"0.25em\" refY=\"5\">" + body + "</symbol></defs>" + use),
      Doc("<defs><symbol id=\"s\" viewBox=\"0 0 10 10\" refX=\"5\" refY=\"5\">" + body + "</symbol></defs>" + use), "em");
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
