#include "SXC/ImmersiveAzeroth/Tables.h"

#include <utility>
#include "SXC/Data/Data.h"

namespace {
    using namespace SXC::Data;

    // ScopeType + ScopeID: where a row applies. Global < Map < Area (zone) < Area (sub-area); the more
    // specific row wins, field by field.
    void Scope(Schema& schema) {
        schema.Enum("ScopeType").Value(0, "Global").Value(1, "Map").Value(2, "Area").Group("Place")
            .Tooltip("Where the row applies. Global < Map < Area (zone) < Area (sub-area): the more specific row wins, field by field.");
        schema.Int("ScopeID").Group("Place").Tooltip("Map.dbc ID or AreaTable ID (ignored for Global).");
    }

    Field& Comment(Schema& schema) {
        return schema.Text("Comment").Tooltip("Free text for the editor (which place the row is); never read.");
    }

    // -1 takes the value from the next, less specific row (or the parent).
    Field& Inherit(Field& field) {
        return field.Value(-1, "inherit").Default(-1);
    }

    // Signed values (temperatures, offsets) inherit at -1000, since -1 is a real value.
    Field& InheritSigned(Field& field) {
        return field.Value(-1000, "inherit").Default(-1000);
    }

    Schema WindProfile() {
        Schema s("WindProfile", "DBFilesClient\\WindProfile.cdbc");
        s.Description("Wind per place. A negative value inherits from the place above. No Global row = no wind.");
        s.Id();
        Scope(s);
        Inherit(s.Float("GroundMin")).Range(0, 1).Group("Strength").Tooltip("Strength range at ground level (0..1).");
        Inherit(s.Float("GroundMax")).Range(0, 1).Group("Strength");
        Inherit(s.Float("AloftMin")).Range(0, 1).Group("Strength").Tooltip("Strength range at and above the aloft height.");
        Inherit(s.Float("AloftMax")).Range(0, 1).Group("Strength");
        Inherit(s.Float("AloftHeight")).Group("Strength").Tooltip("Yards above ground where wind is fully unobstructed.");
        Inherit(s.Float("WeatherInfluence")).Range(0, 1).Tooltip("How far bad weather pushes strength toward the max.");
        Inherit(s.Float("GustStrength")).Range(0, 1).Group("Gusts").Tooltip("Gust amplitude on top of the steady wind.");
        Inherit(s.Float("GustFrequency")).Group("Gusts").Tooltip("Relative, 1 = default.");
        Inherit(s.Float("TerrainWeight")).Range(0, 1).Tooltip("Reserved for terrain shaping.");
        Inherit(s.Float("LockedDirection")).Range(0, 360).Group("Direction").Tooltip("Compass degrees the wind blows toward (0 north, 90 east).");
        Inherit(s.Float("LockStrength")).Range(0, 1).Group("Direction").Tooltip("Pull toward the locked direction (0 = no lock).");
        s.Flags("Flags").Tooltip("Reserved, 0.");
        Comment(s);
        return s;
    }

    Schema GroundEffectDoodadWind() {
        Schema s("GroundEffectDoodadWind", "DBFilesClient\\GroundEffectDoodadWind.cdbc");
        s.Description("Per-doodad grass overrides: a row replaces the automatic values for that doodad.");
        s.Id().Tooltip("GroundEffectDoodad.dbc ID.");
        s.Flags("Flags").Bit(0x1, "no wind").Bit(0x2, "no push").Bit(0x4, "droop when wet (reserved)").Bit(0x8, "water current (reserved)").Bit(0x10, "flip root/tip");
        s.Float("Stiffness").Range(0, 1).Tooltip("0 flexible .. 1 rigid (scales wind and push).");
        s.Float("RootV").Label("Root V").Value(-1, "automatic").Default(-1).Tooltip("Manual bend mapping in texture V; -1 = automatic from the model geometry.");
        s.Float("TipV").Label("Tip V").Value(-1, "automatic").Default(-1);
        return s;
    }

    Schema GroundEffectDoodadDensity() {
        Schema s("GroundEffectDoodadDensity", "DBFilesClient\\GroundEffectDoodadDensity.cdbc");
        s.Description("How dense grass is drawn near the player (instanced grass). The most specific row wins: place first, then doodad and ground effect.");
        s.Id();
        Scope(s);
        s.Uint("DoodadID").Value(0, "every doodad").Tooltip("GroundEffectDoodad.dbc ID.");
        s.Uint("GroundEffectID").Value(0, "any").Tooltip("GroundEffectTexture.dbc ID (what the map designer painted).");
        Inherit(s.Float("Multiplier")).Tooltip("Plants drawn per placed plant (1 = no copies).");
        Inherit(s.Float("Radius")).Tooltip("Yards around the player; full density within half of it.");
        Inherit(s.Float("Spread")).Tooltip("Most yards a copy moves from its plant.");
        return s;
    }

    Schema GroundMaterialSelector() {
        Schema s("GroundMaterialSelector", "DBFilesClient\\GroundMaterialSelector.cdbc");
        s.Description("Where a ground material lies. The most specific row wins: place first, then texture, ground effect, TerrainType, everything.");
        s.Id();
        Scope(s);
        s.Path("TexturePath").Group("What").Tooltip("The painted texture's path; empty = any. Case and / vs \\ don't matter.");
        s.Uint("GroundEffectID").Value(0, "any").Group("What").Tooltip("GroundEffectTexture.dbc ID (the effect painted with the texture).");
        s.Enum("TerrainType").Group("What").Default(-1).Value(-1, "any").Value(0, "Dirt").Value(1, "Metallic").Value(2, "Stone").Value(3, "Snow")
            .Value(4, "Wood").Value(5, "Grass").Value(6, "Leaves").Value(7, "Sand").Value(8, "Soggy").Value(9, "Dusty Grass").Value(10, "None").Value(11, "Water")
            .Tooltip("TerrainType.dbc ID.");
        s.Uint("LiquidType").Value(0, "terrain row").Tooltip("A LiquidType.dbc ID makes this a liquid row: that liquid is this material. Only the place and MaterialID count on liquid rows.");
        s.ForeignKey("MaterialID", "GroundMaterial").Value(0, "no material").Tooltip("0 switches a broader row off (e.g. no snow in one zone).");
        s.Flags("Flags").Tooltip("Reserved, 0.");
        return s;
    }

    Schema GroundMaterial() {
        Schema s("GroundMaterial", "DBFilesClient\\GroundMaterial.cdbc");
        s.Description("What a ground material is. A float of -1 takes the value from the parent (and so on up).");
        s.Id();
        s.String("Name");
        s.ForeignKey("Parent", "GroundMaterial").Value(0, "none").Tooltip("Material whose values a -1 field takes. Feature tables follow the same chain.");
        Inherit(s.Float("RestMoisture")).Range(0, 1).Tooltip("0 bone dry .. 1 soaked: what the texture already shows. Moisture rests here.");
        Inherit(s.Float("Stiffness")).Range(0, 1).Tooltip("0 gives way completely .. 1 rigid. Feet press away (1 - Stiffness) of a cover's depth.");
        s.Flags("Flags").Tooltip("Reserved, 0.");
        Inherit(s.Float("Absorbency")).Range(0, 1).Tooltip("How much outside moisture reaches it: rain on open ground, water nearby. Rock 0.1, soil 0.9.");
        s.Float("Temperature").Value(-1000, "none").Default(-1000).Tooltip("Degrees C of the material itself: a heat source (magma, lava textures).");
        return s;
    }

    Schema SurfaceCover() {
        Schema s("SurfaceCover", "DBFilesClient\\SurfaceCover.cdbc");
        s.Description("Which materials carry a cover (snow, sand, mud), how deep, how it looks and behaves. A float of -1 takes the field from the parent material's row.");
        s.Id();
        s.ForeignKey("MaterialID", "GroundMaterial").Tooltip("The material that carries this cover (one row per material).");
        s.ForeignKey("CoverMaterial", "GroundMaterial").Value(0, "the ground material").Tooltip("The material of what lies on top (snow on grass is still Snow). Its Stiffness decides how far feet press the cover down.");
        Inherit(s.Float("Depth")).Value(0, "no cover").Tooltip("Yards of cover.");
        Inherit(s.Float("MaxSlope")).Range(0, 90).Group("Slope").Tooltip("Degrees: steeper ground holds no cover. Default 45.");
        Inherit(s.Float("SlopeFade")).Range(0, 90).Group("Slope").Tooltip("Degrees below MaxSlope over which the cover thins out. Default 15.");
        Inherit(s.Float("DriftNoise")).Range(0, 1).Group("Shape").Tooltip("Depth variation, share of the depth (0.35 = +-35%).");
        Inherit(s.Float("EdgeBreakup")).Range(0, 1).Group("Shape").Tooltip("0 smooth, rounded edges .. 1 ragged, patchy ones.");
        Inherit(s.Float("Rim")).Group("Trenches").Tooltip("Height of the rim beside a trench, share of the depth.");
        Inherit(s.Float("RelaxSeconds")).Group("Trenches").Tooltip("How long a trench takes to fill back in.");
        s.Color("TintColor").Group("Look").Tooltip("Used with TintStrength.");
        Inherit(s.Float("TintStrength")).Range(0, 1).Group("Look").Tooltip("0 the plain cover colour .. 1 TintColor.");
        s.Path("CoverTexture").Group("Look").Tooltip("A BLP drawn on the cover, one repeat per terrain cell. Empty = from the parent material's row, '-' = none.");
        Inherit(s.Float("Opacity")).ReadOnly().Tooltip("Reserved, not used yet.");
        Inherit(s.Float("Flatten")).ReadOnly().Tooltip("Reserved, not used yet.");
        InheritSigned(s.Float("ZOffset")).Group("Shape").Tooltip("Yards the cover's base sits above (+) or below (-) the terrain. Exactly -1 inherits too.");
        Inherit(s.Float("Wetness")).Range(0, 1).Group("Look").Tooltip("How wet the cover looks at rest: 0 as its texture .. 1 soaked.");
        s.Flags("Flags").Bit(0x1, "prevent underwater").Bit(0x2, "prevent on land").Bit(0x4, "use MCCV").Bit(0x8, "ignore specular map")
            .Tooltip("Flags come from the row that decides Depth, except 0x8, which belongs to the CoverTexture named in the same row.");
        Inherit(s.Float("MeltKeptShare")).Range(0, 1).Group("Snow").Tooltip("Snow covers: the share of the painted depth mild weather melts it down to (at 15 C). 0.6 if no row sets it.");
        InheritSigned(s.Float("MeltGoneTemperature")).Group("Snow").Tooltip("Snow covers: degrees C at which painted snow melts away completely. 25 if no row sets it.");
        Inherit(s.Float("MoistureMin")).Value(2, "off").Range(0, 1).Tooltip("The cover only forms where the ground's moisture is at least this (mud). 2 = off: doesn't depend on moisture.");
        return s;
    }

    Schema DoodadLightProperties() {
        Schema s("DoodadLightProperties", "DBFilesClient\\DoodadLightProperties.cdbc");
        s.Description("What a doodad light is. A value of -1 takes the default (the lights panel's settings).");
        s.Id();
        s.String("Name");
        s.Enum("Type").Value(0, "point").Value(1, "spot");
        s.Color("Color");
        Inherit(s.Float("Intensity")).Tooltip("Times the colour. The stock torches are 1.4 x 0xFF9F49.");
        Inherit(s.Float("Radius")).Tooltip("Yards; the light is exactly 0 here. Default 16.");
        Inherit(s.Float("Falloff")).Tooltip("Shape: 1 smooth (default), higher = tighter around the source.");
        Inherit(s.Float("InnerAngle")).Range(0, 180).Group("Spot").Tooltip("Degrees from the axis at full strength.");
        Inherit(s.Float("OuterAngle")).Range(0, 180).Group("Spot").Tooltip("Degrees from the axis beyond which there is no light.");
        s.Enum("FlickerMode").Default(-1).Value(-1, "default (smooth)").Value(0, "off").Value(1, "smooth").Value(2, "noise").Value(3, "noise steps").Group("Flicker");
        Inherit(s.Float("FlickerSpeed")).Group("Flicker").Tooltip("Times the default pace.");
        Inherit(s.Float("FlickerAmount")).Range(0, 1).Group("Flicker").Tooltip("How much the light dips.");
        s.Flags("Flags").Tooltip("Reserved, 0.");
        return s;
    }

    Schema DoodadLightAssignment() {
        Schema s("DoodadLightAssignment", "DBFilesClient\\DoodadLightAssignment.cdbc");
        s.Description("Where doodad lights go. Several rows per model are fine.");
        s.Id();
        s.Path("ModelPath").Tooltip("The model's path as the lights panel shows it; case and / vs \\ don't matter.");
        s.Enum("AttachType").Value(0, "origin").Value(1, "attachment point").Value(2, "bone").Value(3, "particle emitter").Group("Attachment");
        s.Int("AttachIndex").Group("Attachment").Tooltip("The attachment point ID / bone index / emitter index.");
        s.String("AttachName").ReadOnly().Group("Attachment").Tooltip("Reserved for named attachment points, empty.");
        s.Vector("Offset").Group("Attachment").Tooltip("Yards from the attachment, in the model's space (scaled with the doodad).");
        s.Vector("Direction").Tooltip("Spot lights: where the cone points, in the model's space (0, 0, -1 = down).");
        s.ForeignKey("LightID", "DoodadLightProperties").Value(0, "no light");
        s.Flags("Flags").Bit(0x1, "suppress the model's own lights");
        return s;
    }

    Schema AreaClimate() {
        Schema s("AreaClimate", "DBFilesClient\\AreaClimate.cdbc");
        s.Description("The air's baseline per place: temperature through the day and the year, humidity, fogginess, day length.");
        s.Id();
        Scope(s);
        InheritSigned(s.Float("DayTemp")).Group("Temperature").Tooltip("Degrees C at the warmest time of day (15:00).");
        InheritSigned(s.Float("NightTemp")).Group("Temperature").Tooltip("Degrees C at the coldest (05:00).");
        Inherit(s.Float("SeasonAmplitude")).Group("Temperature").Tooltip("Degrees C between midsummer and midwinter (Elwynn ~12, jungle ~2, Outland 0).");
        Inherit(s.Float("SeasonOffset")).Group("Temperature").Tooltip("Months the seasons are shifted (6 = flipped).");
        Inherit(s.Float("Humidity")).Range(0, 1).Tooltip("0 dry air .. 1 saturated: humid places dry slowly.");
        s.Flags("Flags").Tooltip("Reserved, 0.");
        Inherit(s.Float("Fogginess")).Range(0, 1).Tooltip("How foggy the place is by nature; drives area fog, boosts mist.");
        Comment(s);
        Inherit(s.Float("DayLength")).Range(0, 3.5).Tooltip("Hours the sunrise comes earlier (and the sunset later) at midsummer; 0 = stock day length all year.");
        return s;
    }

    Schema SkyTime() {
        Schema s("SkyTime", "DBFilesClient\\SkyTime.cdbc");
        s.Description("The sky's time per place: pinned (always night above Karazhan) or shifted (a time zone).");
        s.Id();
        Scope(s);
        Inherit(s.Float("PinnedHour")).Value(-2, "not pinned").Range(0, 24).Tooltip("The sky stays at this hour (23.5 = 23:30).");
        InheritSigned(s.Float("OffsetHours")).Tooltip("Hours added to the time where it isn't pinned.");
        Inherit(s.Float("BlendSeconds")).Tooltip("How long the sky takes to move to a new rule at a border. Default 10.");
        s.Flags("Flags").Tooltip("Reserved, 0.");
        Comment(s);
        s.Int("PinnedDay").Value(0, "inherit").Value(-1, "realm date").Range(1, 365).Tooltip("The place's date is pinned to this day of the year (172 = 21 June).");
        return s;
    }

    Schema SkyProfile() {
        Schema s("SkyProfile", "DBFilesClient\\SkyProfile.cdbc");
        s.Description("The place's sky: its set of bodies, skyboxes, north, and the lore clock.");
        s.Id();
        Scope(s);
        s.ForeignKey("BodySet", "SkyBodySet").Value(0, "inherit").Value(-1, "the client's own sun and moons");
        s.Enum("HideZoneSkybox").Default(-1).Value(-1, "inherit").Value(0, "no").Value(1, "yes").Group("Skyboxes").Tooltip("Hide this place's M2 skybox (the strongest one).");
        s.Enum("HideAllSkyboxes").Default(-1).Value(-1, "inherit").Value(0, "no").Value(1, "yes").Group("Skyboxes");
        InheritSigned(s.Float("NorthOffset")).Tooltip("Degrees a map's north is turned (custom maps).");
        s.Flags("Flags").Tooltip("Reserved, 0.");
        Comment(s);
        s.Int("LoreYear").Default(-1000000).Value(-1000000, "inherit").Group("Lore clock").Tooltip("The story's year here (0 = the Dark Portal opens, 27 = WotLK). Nowhere set: ini LoreYear (27).");
        Inherit(s.Float("LoreWobbleYears")).Group("Lore clock").Tooltip("Lore time swings this many years over the place's year; 0 = none.");
        s.Enum("LoreWobbleMode").Default(-1).Value(-1, "inherit").Value(0, "around").Value(1, "before").Value(2, "after").Group("Lore clock");
        Inherit(s.Float("LoreYearsPerDay")).Group("Lore clock").Tooltip("0 = lore time frozen at LoreYear; > 0 = running, lore years per realm day from LoreStartDate on.");
        s.Int("LoreStartDate").Value(0, "inherit").Group("Lore clock").Tooltip("Running lore time: the realm date (yyyymmdd) on which the place is at LoreYear.");
        return s;
    }

    Schema SkyBodySet() {
        Schema s("SkyBodySet", "DBFilesClient\\SkyBodySet.cdbc");
        s.Description("A set of sky bodies (\"Azeroth\"), optionally extending another set.");
        s.Id();
        s.String("Name");
        s.ForeignKey("ParentSet", "SkyBodySet").Value(0, "none").Tooltip("A set this one extends: its members plus these.");
        s.Flags("Flags");
        Comment(s);
        return s;
    }

    Schema SkyBodySetMember() {
        Schema s("SkyBodySetMember", "DBFilesClient\\SkyBodySetMember.cdbc");
        s.Description("The bodies in each set.");
        s.Id();
        s.ForeignKey("SetID", "SkyBodySet");
        s.ForeignKey("BodyID", "SkyBody");
        s.Flags("Flags").Bit(0x1, "off (removes a parent's body from this set)");
        Comment(s);
        return s;
    }

    Schema SkyBody() {
        Schema s("SkyBody", "DBFilesClient\\SkyBody.cdbc");
        s.Description("The sun, the moons and any custom body, drawn instead of the client's own.");
        s.Id();
        s.String("Name").Tooltip("What Lua will use.");
        s.Path("Texture").Group("Look").Tooltip("BLP path; empty = the stock texture of its StockRole.");
        s.Enum("StockRole").Value(0, "none").Value(1, "sun").Value(2, "White Lady").Value(3, "Blue Child").Tooltip("The stock body it replaces / starts from.");
        s.Float("Size").Value(0, "the client's").Group("Look").Tooltip("Angular diameter in degrees (horizon to zenith = 90).");
        s.Color("Color", ColorFormat::Argb).Value(static_cast<double>(0xFFFFFFFFu), "none").Group("Look").Tooltip("Tint; 0xFFFFFFFF = none (the client's colour and fade).");
        s.Enum("Blend").Value(0, "alpha").Value(1, "additive").Group("Look").Tooltip("Additive glows: added to the sky behind it; uses our own draw.");
        s.Int("Order").Group("Look").Tooltip("Draw order: lower first = behind; equal: file order.");
        s.ForeignKey("PathID", "SkyPath").Value(0, "stock path").Group("Motion");
        s.Float("PeriodDays").Value(0, "hangs still").Group("Motion").Tooltip("One trip along the path, in days. 1 = daily: follows the place's time of day.");
        s.Float("PhaseDays").Group("Motion").Tooltip("Offset along the path, in days.");
        s.Float("VisiblePeriodDays").Value(0, "always").Group("Visibility").Tooltip("In the sky VisibleDays out of every VisiblePeriodDays. Every third night: 3, 1, 0.5.");
        s.Float("VisibleDays").Group("Visibility");
        s.Float("VisibleOffsetDays").Group("Visibility").Tooltip("The window starts this many days after midnight of day 0 (1970-01-01).");
        s.Int("LitMode").ReadOnly().Group("Phases").Tooltip("Phases: later (2c.3).");
        s.Float("LitPeriodDays").ReadOnly().Group("Phases");
        s.Color("LightColor").Group("World light").Tooltip("With flag 8: the world light's colour while this body drives it.");
        s.Float("LightStrength").Range(0, 1).Group("World light").Tooltip("0 = the zone's colour as is, 1 = fully times LightColor.");
        s.Flags("Flags").Bit(0x1, "own colour").Bit(0x2, "no client glare").Bit(0x4, "our own draw").Bit(0x8, "drives the world light");
        Comment(s);
        s.Flags("VisibleEase").Bit(0x1, "fade").Bit(0x2, "scale").Bit(0x4, "sink").Group("Visibility").Tooltip("How it arrives and leaves; 0 = pops.");
        s.Float("VisibleEaseSeconds").Group("Visibility").Tooltip("Seconds of sky time the arrival and the leaving take.");
        s.Enum("Clock").Value(0, "realm").Value(1, "lore").Group("Motion").Tooltip("Lore: PeriodDays and PhaseDays count lore years.");
        s.Float("DayVisibility").Value(-1, "auto").Default(-1).Range(0, 1).Group("Look").Tooltip("Our own draw: how much of it shows at full day. Auto: 1 for the sun, 0.25 for anything else.");
        s.Float("Rotation").Group("Look").Tooltip("The picture turned clockwise, in degrees (any value but 0 uses our own draw).");
        s.Float("RotationPerDay").Group("Look").Tooltip("Degrees per realm day on top.");
        return s;
    }

    Schema SkyPath() {
        Schema s("SkyPath", "DBFilesClient\\SkyPath.cdbc");
        s.Description("How sky bodies move: the stock curves, an orbit, keyframes (SkyPathKey) or following another body.");
        s.Id();
        s.String("Name");
        s.Enum("Type").Value(0, "stock").Value(1, "orbit").Value(2, "keyframes").Value(3, "follow");
        s.Int("Frame").ReadOnly().Tooltip("Later (a star frame waits for our own star field).");
        s.Float("RiseAzimuth").Range(0, 360).Group("Orbit").Tooltip("Compass degrees where it comes up; the map's NorthOffset is added.");
        s.Float("Lift").Group("Orbit").Tooltip("Degrees the whole circle is raised (Lift >= Height = never sets).");
        s.Float("Height").Group("Orbit").Tooltip("Highest elevation before Lift (peaks over RiseAzimuth + 90).");
        s.ForeignKey("FollowBody", "SkyBody").Group("Follow").Tooltip("The leader (up to 4 deep).");
        s.Float("SeparationMin").Group("Follow").Tooltip("Degrees from the leader, swinging from Min to Max and back over SeparationDays.");
        s.Float("SeparationMax").Group("Follow");
        s.Float("SeparationDays").Group("Follow").Tooltip("0 = always Min.");
        s.Float("SeparationAngle").Range(0, 360).Group("Follow").Tooltip("Which way from the leader: 0 below it, 90 to its right, 180 above, 270 to its left.");
        s.Float("WobbleDegrees").Group("Modifiers").Tooltip("Swings the body up/down over WobbleDays (any path).");
        s.Float("WobbleDays").Group("Modifiers");
        s.Float("DriftDegreesPerDay").Group("Modifiers").Tooltip("Turns the whole path around the zenith.");
        s.Float("SizePulse").Group("Modifiers").Tooltip("Size x (1 +- SizePulse) over SizePulseDays.");
        s.Float("SizePulseDays").Group("Modifiers");
        s.Flags("Flags");
        Comment(s);
        return s;
    }

    Schema SkyPathKey() {
        Schema s("SkyPathKey", "DBFilesClient\\SkyPathKey.cdbc");
        s.Description("The points of a keyframe path; on a follow path, relative to the leader.");
        s.Id();
        s.ForeignKey("PathID", "SkyPath");
        s.Float("Time").Range(0, 1).Tooltip("0..1 along the path (one trip = the body's PeriodDays).");
        s.Float("Azimuth").Tooltip("Compass degrees (follow paths: degrees to the leader's right).");
        s.Float("Elevation").Tooltip("Degrees above the horizon (follow paths: above the leader).");
        s.Float("Size").Value(0, "1").Tooltip("Multiplier on the body's Size.");
        s.Flags("Flags");
        Comment(s);
        s.Float("LeaderBrightness").Value(0, "1").Group("Follow").Tooltip("The leader's brightness at this key.");
        s.Enum("Order").Value(-1, "behind the leader").Value(0, "by the bodies' Order").Value(1, "in front of the leader").Group("Follow");
        return s;
    }

    Schema FogSettings() {
        Schema s("FogSettings", "DBFilesClient\\FogSettings.cdbc");
        s.Description("How the ground fog behaves and looks, per place and fog kind. All floats: -1 takes the field from the next row.");
        s.Id();
        Scope(s);
        s.Enum("Kind").Value(0, "all kinds").Value(1, "mist").Value(2, "water fog").Value(3, "steam").Value(4, "area fog").Value(5, "haze").Group("Place");
        Inherit(s.Float("Strength")).Tooltip("Multiplies the kind's target from its rules (0 = never here).");
        Inherit(s.Float("Density")).Tooltip("Extinction per yard at the ground at full amount (mist ~0.06).");
        Inherit(s.Float("Thickness")).Tooltip("Yards: layer height at full amount.");
        Inherit(s.Float("BuildUp")).Group("Timing").Tooltip("Half-life (s) toward a higher target.");
        Inherit(s.Float("BurnOff")).Group("Timing").Tooltip("Half-life (s) toward a lower target.");
        Inherit(s.Float("WindThin")).Range(0, 1).Group("Wind").Tooltip("Wind strength above which it can't settle.");
        Inherit(s.Float("WindDrift")).Range(0, 1).Group("Wind").Tooltip("How strongly the wind moves it.");
        Inherit(s.Float("Patchiness")).Range(0, 1).Tooltip("0 smooth .. 1 patchy.");
        Inherit(s.Float("Weight")).Range(0, 1).Tooltip("0 rises and thins upward .. 1 heavy: flat-topped, flows downhill, pools in hollows.");
        s.Enum("ColorMode").Default(-1).Value(-1, "inherit").Value(0, "stock").Value(1, "hue").Value(2, "replace").Group("Colour")
            .Tooltip("Stock: the zone's fog colour. Hue: its brightness with Color's hue and saturation. Replace: Color as is.");
        s.Color("Color", ColorFormat::Float3).Value(-1, "inherit").Default(-1).Group("Colour");
        Inherit(s.Float("Saturation")).Range(0, 1).Group("Colour").Tooltip("0 grey .. 1 as is.");
        Inherit(s.Float("Brightness")).Group("Colour").Tooltip("Multiplier.");
        s.Flags("Flags").Tooltip("Reserved, 0.");
        Comment(s);
        return s;
    }
}

namespace SXC::ImmersiveAzeroth {
    void RegisterTables() {
        Schema (*const schemas[])() = {
            AreaClimate, DoodadLightAssignment, DoodadLightProperties, FogSettings, GroundEffectDoodadDensity, GroundEffectDoodadWind,
            GroundMaterial, GroundMaterialSelector, SkyBody, SkyBodySet, SkyBodySetMember, SkyPath, SkyPathKey, SkyProfile, SkyTime,
            SurfaceCover, WindProfile,
        };

        for (Schema (*schema)() : schemas) {
            SXC::Data::Register(schema());
        }
    }
}
