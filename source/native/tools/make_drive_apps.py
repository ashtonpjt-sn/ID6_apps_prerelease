"""Generate RHMI resources for the standalone ShiftDeck and MHD Maps apps."""
from pathlib import Path
from io import BytesIO
from xml.etree import ElementTree as ET
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo
from PIL import Image, ImageDraw, ImageFont
import shift_art as art

ROOT = Path(__file__).resolve().parents[1] / "plugin"


def save(doc, name):
    ET.indent(doc, space="  ")
    ET.ElementTree(doc).write(ROOT / name, encoding="utf-8", xml_declaration=True)


def entry(name, title, weight, connector, root_id, images=False):
    manifest = ET.parse(ROOT / "BoostDeck.manifest").getroot()
    manifest.find("name").text = name
    manifest.find("file").text = "{pluginDir}/" + name
    manifest.find("config/dependencies/pluginAppConnector/id").text = str(connector)
    params = manifest.find("config/parameters")
    params.find("DescFile").text = f"{{pluginDir}}/{name}.xml"
    if images:
        params.find("ImageDBFile").text = f"{{pluginDir}}/{name}_ALL_Images.zip"
    else:
        params.remove(params.find("ImageDBFile"))
    save(manifest, f"{name}.manifest")

    doc = ET.Element("pluginApps")
    app = ET.SubElement(doc, "pluginApp", applicationType="VehicleInformation",
                        applicationWeight=str(weight), text=title)
    entry_button = ET.SubElement(app, "entryButton", id="1", action="10")
    ET.SubElement(ET.SubElement(entry_button, "properties"), "property", id="23", value="2")
    actions = ET.SubElement(app, "actions")
    ET.SubElement(actions, "hmiAction", id="10", target=str(root_id))
    models = ET.SubElement(app, "models")
    states = ET.SubElement(app, "hmiStates")
    state = ET.SubElement(states, "hmiState", id=str(root_id), text=title)
    return doc, actions, models, states, state


def position(component, x, y, width, height):
    properties = ET.SubElement(component, "properties")
    for prop, value in ((20,x),(21,y),(9,width),(10,height)):
        ET.SubElement(properties, "property", id=str(prop), value=str(value))


def button(group, identifier, action, text):
    ET.SubElement(group, "button", id=str(identifier), action=str(action), text=text)


def shift_app():
    name="ShiftDeck"
    doc, actions, models, states, page = entry(name,"ShiftDeck",207,104,100,images=True)
    ET.SubElement(actions,"hmiAction",id="40",target="101")
    for i in range(len(art.TARGETS)):
        ET.SubElement(actions,"raAction",id=str(41+i))
    ET.SubElement(models,"raDataModel",id="53",modelType="Richtext")

    options=ET.SubElement(page,"optionComponents")
    button(options,30,40,"Shift light setting")
    content=ET.SubElement(page,"components")

    def graphic(model, asset, x, y, w, h):
        ET.SubElement(models,"imageIdModel",id=str(model),imageId=str(asset))
        image=ET.SubElement(content,"image",id=str(200+model-54),model=str(model))
        position(image,x,y,w,h)
        props=image.find("properties")
        ET.SubElement(props,"property",id="2",value="false")
        ET.SubElement(props,"property",id="3",value="true")

    graphic(54,art.BAR_BASE,52,287,art.BAR_WIDTH,90)
    graphic(55,art.LIGHT_BASE,52,10,1104,76)
    graphic(56,art.TARGET_BASE+art.DEFAULT_INDEX,1003,113,153,135)
    graphic(57,art.SHIFT_OFF,454,215,350,42)
    for i in range(4):
        graphic(60+i,art.DIGIT_BASE+(10 if i in (1,2) else 11),501+i*64,108,64,100)
    graphic(64,art.SPEED_LEGEND,53,108,200,179)
    graphic(65,art.RPM_LEGEND,764,169,68,35)
    graphic(66,art.SCALE,52,377,art.BAR_WIDTH,42)
    graphic(67,art.LOWER,52,420,1104,10)
    graphic(68,art.MARK_BASE+art.DEFAULT_INDEX,52,278,art.BAR_WIDTH,9)
    for i in range(3):
        graphic(70+i,art.DIGIT_BASE+(10 if i==2 else 11),53+i*67,139,67,109)
    # Gear estimate between speed and RPM ("-" until learned).
    graphic(75,art.GEAR_LEGEND,330,108,120,179)
    graphic(74,art.DIGIT_BASE+10,330,139,67,109)

    settings=ET.SubElement(states,"hmiState",id="101",parentId="100",text="Shift light setting")
    items=ET.SubElement(settings,"components")
    ET.SubElement(items,"label",id="300",model="53")
    for i,rpm in enumerate(art.TARGETS):
        button(items,301+i,41+i,f"{rpm:,} RPM")

    save(doc,f"{name}.xml")
    art.save_assets(ROOT/f"{name}_ALL_Images.zip",ROOT.parent/"src"/"shift_assets.h")


def map_app():
    name="MhdMaps"
    doc, actions, models, _, page = entry(name,"MHD Maps",208,103,100,images=True)
    ET.SubElement(models,"raDataModel",id="50",modelType="Richtext")
    content=ET.SubElement(page,"components")
    def graphic(component, model, asset, x, y, w, h):
        ET.SubElement(models,"imageIdModel",id=str(model),imageId=str(asset))
        image=ET.SubElement(content,"image",id=str(component),model=str(model))
        position(image,x,y,w,h)
        ET.SubElement(image.find("properties"),"property",id="2",value="false")
        ET.SubElement(image.find("properties"),"property",id="3",value="true")

    graphic(200,60,56000,420,35,360,48)
    graphic(201,61,56001,174,118,852,192)
    ET.SubElement(models,"imageIdModel",id="62",imageId="56002")
    for slot in range(1,5):
        ET.SubElement(actions,"raAction",id=str(20+slot))
        # iDrive places native button text at the top left. Keep its focus
        # outline and action, and draw the centered labels on the tile art.
        tile=ET.SubElement(content,"button",id=str(150+slot),action=str(20+slot),imageModel="62")
        position(tile,174+(slot-1)*220,118,192,192)
    position(ET.SubElement(content,"label",id="150",model="50"),174,337,850,53)
    save(doc,f"{name}.xml")

    arial=Path("/System/Library/Fonts/Supplemental/Arial.ttf")
    font=ImageFont.truetype(str(arial) if arial.is_file() else "DejaVuSans.ttf",39)
    logo=Image.new("RGBA",(360,48))
    ImageDraw.Draw(logo).text((180,24),"MHD Maps",font=font,anchor="mm",fill="#f0f4fb")
    tiles=Image.new("RGBA",(852,192))
    draw=ImageDraw.Draw(tiles)
    for slot,x in enumerate(range(0,4*220,220),1):
        draw.rounded_rectangle((x,0,x+191,191),radius=9,fill="#192331",outline="#475c74",width=2)
        draw.text((x+96,96),f"Map {slot}",font=font,anchor="mm",fill="#f0f4fb")
    with ZipFile(ROOT/f"{name}_ALL_Images.zip","w",ZIP_DEFLATED) as archive:
        transparent_icon=Image.new("RGBA",(1,1))
        for identifier,image in ((56000,logo),(56001,tiles),(56002,transparent_icon)):
            data=BytesIO()
            image.save(data,format="PNG",optimize=True)
            archive.writestr(ZipInfo(f"{identifier}.png"),data.getvalue(),ZIP_DEFLATED)


if __name__=="__main__":
    shift_app()
    map_app()
