#include <QtMath>
#include <QElapsedTimer>
#include <marble/ViewportParams.h>
#include <marble/GeoDataLinearRing.h>
#include <marble/GeoDataLatLonAltBox.h>
#include "LocalMapLogic.h"
#include "MapRobotObject.h"
#include "MapLibraryHelpers.h"

using namespace Marble;
using namespace MapAbstraction;

//TODO - fix errors crossing timeline
namespace
{
    inline qreal distanceSphere(qreal lon1, qreal lat1, qreal lon2, qreal lat2)
    {   //TODO - taken from Marble.
        qreal h1 = sin(0.5 * (lat2 - lat1));
        qreal h2 = sin(0.5 * (lon2 - lon1));
        qreal d = h1 * h1 + cos(lat1) * cos(lat2) * h2 * h2;
        return 2.0 * atan2(sqrt(d), sqrt(1.0 - d));
    }

    Marble::GeoDataLinearRing ringFromBox(Marble::GeoDataLatLonBox box)
    {
        QList<Marble::GeoDataCoordinates> coordinates;
        coordinates.append(Marble::GeoDataCoordinates(box.west(), box.north()));
        coordinates.append(Marble::GeoDataCoordinates(box.west(), box.south()));
        coordinates.append(Marble::GeoDataCoordinates(box.east(), box.south()));
        coordinates.append(Marble::GeoDataCoordinates(box.east(), box.north()));
        Marble::GeoDataLinearRing ring;
        foreach (const Marble::GeoDataCoordinates& coord, coordinates)
        {
            const qreal lon = coord.longitude();
            const qreal lat = coord.latitude();
            ring.append(Marble::GeoDataCoordinates(lon, lat));
        }
        ring.append(ring.first());
        return ring;
    }

    Marble::GeoDataCoordinates rotatedCoords(Marble::GeoDataCoordinates coords,
                                             Marble::GeoDataCoordinates rotationPoint,
                                             qreal rotation)
    {
        const qreal cosRotation = cos(rotation);
        const qreal sinRotation = sin(rotation);

        qreal centerLat = rotationPoint.latitude();
        qreal centerLon = rotationPoint.longitude();
        qreal lon = coords.longitude();
        qreal lat = coords.latitude();

        if (centerLat == M_PI/2)
            centerLat = M_PI/2 - 0.000001;

        qreal latDist = (lat - centerLat) / cos(centerLat); //adjusted
        qreal lonDist = (lon - centerLon) * cos(centerLat);

        const qreal rotatedLon = (lon - centerLon) * cosRotation + latDist * sinRotation + centerLon;
        const qreal rotatedLat = centerLat - lonDist * sinRotation + (lat - centerLat) * cosRotation;
        return Marble::GeoDataCoordinates(rotatedLon, rotatedLat);
    }

    Marble::GeoDataLinearRing imageRing(Marble::GeoDataLatLonBox box, qreal rotation)
    {
        QList<Marble::GeoDataCoordinates> coordinates;
        coordinates.append(Marble::GeoDataCoordinates(box.west(), box.north()));
        coordinates.append(Marble::GeoDataCoordinates(box.west(), box.south()));
        coordinates.append(Marble::GeoDataCoordinates(box.east(), box.south()));
        coordinates.append(Marble::GeoDataCoordinates(box.east(), box.north()));

        Marble::GeoDataLinearRing ring;
        foreach (const Marble::GeoDataCoordinates& coord, coordinates)
        {
            ring.append(rotatedCoords(coord, box.center(), rotation));
        }
        ring.append(ring.first());
        return ring;
    }

    void repositionBox(Marble::GeoDataLatLonBox &targetBox,
                       Marble::GeoDataLatLonBox &sourceBox,
                       qreal latitudeDelta, qreal longitudeDelta)
    {
        targetBox.setNorth(sourceBox.north() + latitudeDelta);
        targetBox.setSouth(sourceBox.south() + latitudeDelta);
        targetBox.setEast(sourceBox.east() + longitudeDelta);
        targetBox.setWest(sourceBox.west() + longitudeDelta);
    }

    const qreal optimizationScale = 4.0;
}

LocalMapLogic::LocalMapLogic(MarbleModel *model, MarbleMap *map)
    : mModel(model), mMap(map), mRotation(0)
{
}

GeoDataLatLonBox LocalMapLogic::currentBox() const
{
    return mBox;
}

QImage LocalMapLogic::currentIcon() const
{
    return mIcon;
}

GeoDataLinearRing LocalMapLogic::imageArea() const
{
    return imageRing(mOriginalBox, mRotation);
}

GeoDataLinearRing LocalMapLogic::currentBoxBorder() const
{
    return ringFromBox(mBox);
}

void LocalMapLogic::setOverlay(const QString &filePath, qreal resolution,
                               const GeoDataCoordinates &center, qreal rotation, QPointF origin)
{
    Q_UNUSED(origin);
    mMapResolution = resolution;
    mOrigin = QPointF(0,0);// origin; //TODO
    mIcon.load(filePath);
    mIcon.convertToFormat(QImage::Format_ARGB32);
    mCurrentScaleFactor = 1.0;

    const int blackAreaTranluscency = 100;
    mIcon.setColor(255, qRgba(0, 0, 0, blackAreaTranluscency)); //TODO - assumes black at 255 index in color table
    mOriginalIcon = mIcon;
    mLowResOriginalIcon = mOriginalIcon.scaled(mIcon.width()/optimizationScale,
                                               mIcon.height()/optimizationScale);

    mBox = fabricateBox(center);
    mOriginalBox = mBox;
    mRotation = rotation;
    restoreImageQuality(); //TODO
}

Marble::GeoDataLatLonBox LocalMapLogic::fabricateBox(const GeoDataCoordinates &center) const
{
    Marble::GeoDataLatLonBox box;
    QSizeF geoSize = localMapGeoScale(center);
    box.setBoundaries(center.latitude() + geoSize.height()/2,
                      center.latitude() - geoSize.height()/2,
                      center.longitude() + geoSize.width()/2,
                      center.longitude() - geoSize.width()/2);
    return box;
}

bool LocalMapLogic::handleTouchEvent(QTouchEvent *event,
                                     qreal touchLon, qreal touchLat)
{
    if (event->touchPoints().isEmpty())
        return false;

    QPointF geoPos(touchLon, touchLat);
    QPointF pos = event->touchPoints().first().pos();
    if (event->type() == QEvent::TouchBegin)
    {
        return handleMouse(geoPos, pos, QEvent::MouseButtonPress, Qt::LeftButton);
    }
    if (event->type() == QEvent::TouchUpdate)
    {
        return handleMouse(geoPos, pos, QEvent::MouseMove, Qt::LeftButton);
    }
    if (event->type() == QEvent::TouchEnd)
    {
        return handleMouse(geoPos, pos, QEvent::MouseButtonRelease, Qt::LeftButton);
    }
    return false;
}

bool LocalMapLogic::handleMouseEvent(QMouseEvent *event,
                                     qreal mouseLon, qreal mouseLat)
{
    return handleMouse(QPointF(mouseLon, mouseLat), QPointF(event->x(), event->y()),
                       event->type(), event->button());
}

bool LocalMapLogic::handleMouse(QPointF geoPos, QPointF pos,
                                QEvent::Type type, Qt::MouseButton button)
{
    //TODO - refactor statics out, move to an event handler class
    static bool grabbed = false;
    static bool rotating = false;
    static GeoDataLatLonBox grabBox;
    static GeoDataLatLonBox grabOriginalBox;
    static GeoDataCoordinates grabPoint;
    static qreal lastRotationMouseY;
    qreal mouseLon = geoPos.x();
    qreal mouseLat = geoPos.y();

    GeoDataLinearRing imageHitbox = imageArea();
    bool hitImage = imageHitbox.contains(GeoDataCoordinates(mouseLon, mouseLat));

    /* MouseButtonPress - should we start moving or rotating the image icon? */
    if (type == QEvent::MouseButtonPress)
    {
        if (!hitImage)
        {
            return false;
        }

        if (button == Qt::RightButton)
        {
            if (!rotating)
            {
                rotating = true;
                lastRotationMouseY = pos.y();
                return true;
            }
        }

        if (button == Qt::LeftButton)
        {
            //Which part of the image was clicked - rotate or move?
            QVector<QPolygonF*> vector;
            mMap->viewport()->screenCoordinates(imageHitbox, vector);

            qreal x, y;
            mMap->viewport()->screenCoordinates(imageHitbox.latLonAltBox().center(), x, y);
            QPointF centerPoint(x, y);

            if (vector.size() == 1)
            {
                QPolygonF *imagePolygon = vector.first();
                if (imagePolygon)
                {
                    QPointF maxDistancePoint;
                    qreal maxDistance = 0;
                    foreach (QPointF p, *imagePolygon)
                    {
                        QLineF line(p, centerPoint);
                        qreal distance = line.length();
                        if (distance > maxDistance)
                        {
                            maxDistance = distance;
                            maxDistancePoint = p;
                        }
                    }

                    qreal a, b;
                    mMap->viewport()->screenCoordinates(mouseLon, mouseLat, a, b);
                    QPointF mousePoint(a, b);
                    QLineF lineToMouse(mousePoint, centerPoint);
                    if (lineToMouse.length() >= 0.7 * maxDistance)
                    {   //rotation
                        if (!rotating)
                        {
                            rotating = true;
                            lastRotationMouseY = pos.y();
                            return true;
                        }
                    }
                }
            }

            if (!rotating && !grabbed)
            {
                grabbed = true;
                grabPoint.setLongitude(mouseLon);
                grabPoint.setLatitude(mouseLat);
                grabBox = mBox;
                grabOriginalBox = mOriginalBox;
                return true;
            }
        }

        return false;
    }

    /* MouseButtonRelease - should we stop moving or rotating the image icon? */
    if (type == QEvent::MouseButtonRelease)
    {
        if (button == Qt::LeftButton || button == Qt::RightButton)
        {
            restoreImageQuality();
            grabbed = false;
            rotating = false;
            return true;
        }

        return false;
    }

    /* MouseButtonMove - should we keep moving or rotating the image icon? */
    if (type == QEvent::MouseMove)
    {
        if (grabbed)
        {
            qreal longitudeDelta = mouseLon - grabPoint.longitude();
            qreal latitudeDelta = mouseLat - grabPoint.latitude();
            repositionBox(mBox, grabBox, latitudeDelta, longitudeDelta);
            repositionBox(mOriginalBox, grabOriginalBox, latitudeDelta, longitudeDelta);
            if (mCurrentScaleFactor != optimizationScale)
            {   //we don't need to rotate
                reduceImageQuality();
            }
            emit requiresUpdate();
            return true;
        }

        if (rotating)
        {
            const qreal quant = M_PI/400;
            qreal yDelta = pos.y() - lastRotationMouseY;
            lastRotationMouseY = pos.y();
            qreal angleDiff = yDelta * quant;
            mRotation += angleDiff;
            reduceImageQuality();
            emit requiresUpdate();
            return true;
        }
        return false;
    }

    return false;
}

void LocalMapLogic::restoreImageQuality()
{
    if (optimizationScale != 1.0)
    {   //Restore full quality image
        QTransform rotation;
        rotation.rotate(qRadiansToDegrees(mRotation));
        mIcon = mOriginalIcon.transformed(rotation, Qt::SmoothTransformation);
        mCurrentScaleFactor = 1.0;
        mBox = fabricateBox(mBox.center());
        /*
        qDebug("Rotation %f, center %f, %f", mRotation, mBox.center().longitude(GeoDataCoordinates::Degree),
               mBox.center().latitude(GeoDataCoordinates::Degree));
        */
        emit requiresUpdate();
    }
}

void LocalMapLogic::reduceImageQuality()
{
    if (optimizationScale != 1.0)
    {
        //QElapsedTimer timer;
        //timer.start();
        QTransform rotation;
        rotation.rotate(qRadiansToDegrees(mRotation));
        mIcon = mLowResOriginalIcon.transformed(rotation, Qt::FastTransformation);
        mCurrentScaleFactor = optimizationScale;
        mBox = fabricateBox(mBox.center());
        //("%d, Reduce took %lld usec", QDateTime::currentDateTime().time().second(), timer.nsecsElapsed()/1000);
    }
}

QSizeF LocalMapLogic::localMapGeoScale(const GeoDataCoordinates &center, bool original) const
{
    QSizeF scale = original ? originalMapScale() : localMapScale();

    qreal metersInRadianOfLongitude = mModel->planetRadius();
    qreal radiansHeight = scale.height() / metersInRadianOfLongitude;

    //Latitude circles are smaller as latitude moves away from the equator: cos(lat)
    qreal metersInRadianOfLatitude = cos(center.latitude()) * mModel->planetRadius();
    qreal radiansWidth = scale.width() / metersInRadianOfLatitude;

    return QSizeF(radiansWidth, radiansHeight);
}

QSizeF LocalMapLogic::localMapScale() const
{
    return QSizeF(mIcon.width() * mMapResolution * mCurrentScaleFactor,
                  mIcon.height() * mMapResolution * mCurrentScaleFactor);
}

QSizeF LocalMapLogic::originalMapScale() const
{
    return QSizeF(mOriginalIcon.width() * mMapResolution,
                  mOriginalIcon.height() * mMapResolution);
}

qreal LocalMapLogic::geoLocalizeRotation(qreal rotation) const
{
    //qWarning("Geolocalize rotation %f %f %f", rotation, mRotation, rotation - mRotation);
    return rotation - mRotation;
}

qreal LocalMapLogic::globalToLocalRotation(qreal rotation) const
{
    return rotation + mRotation;
}

GeoDataCoordinates LocalMapLogic::geoLocalizePoint(const QPointF &point) const
{
    //qWarning("localizing point %f, %f", point.x(), point.y());
    QSizeF scaleGlobal = localMapGeoScale(mBox.center(), true);
    QSizeF scaleLocal = originalMapScale();
    if (scaleLocal.width() == 0 || scaleLocal.height() == 0)
    {
        qWarning("Invalid map with a zero dimension, things won't work");
        return GeoDataCoordinates();
    }
    qreal xAspect = (point.x() - mOrigin.x())/scaleLocal.width();
    qreal yAspect = (point.y() - mOrigin.y())/scaleLocal.height();
    qreal lon = xAspect * scaleGlobal.width() + mOriginalBox.west();
    qreal lat = yAspect * scaleGlobal.height() + mOriginalBox.south();

    //Rotate the point
    GeoDataCoordinates coords = rotatedCoords(GeoDataCoordinates(lon, lat),
                                              mOriginalBox.center(),
                                              mRotation);
    //qWarning("localized point to %f, %f", coords.latitude(), coords.longitude());
    return coords;
}

QPointF LocalMapLogic::robotToLocal(const QPointF &point, MapAbstraction::MapRobotObjectConstPtr robot) const
{
    QPointF robotLocalPosition;
    globalToLocal(MapLibraryHelpers::transformCoords(robot->coords()), robotLocalPosition);

    qreal transX = robotLocalPosition.x();
    qreal transY = robotLocalPosition.y();
    qreal alph = -robot->orientation() - mRotation;
    qreal xRot = point.x()*cos(alph) + point.y()*sin(alph);
    qreal yRot = -point.x()*sin(alph) + point.y()*cos(alph);
    return QPointF(xRot + transX, yRot + transY);
}

bool LocalMapLogic::globalToLocal(const GeoDataCoordinates &coords,
                                  QPointF &outPoint) const
{
    QSizeF scaleGlobal = localMapGeoScale(mBox.center(), true);
    if (scaleGlobal.width() == 0 || scaleGlobal.height() == 0)
    {
        qWarning("Invalid map with a zero dimension, things won't work");
        outPoint.setX(0);
        outPoint.setY(0);
        return false;
    }

    GeoDataCoordinates c = rotatedCoords(coords, mOriginalBox.center(), -mRotation);
    qreal lonDist = c.longitude() - mOriginalBox.west();
    qreal latDist = c.latitude() - mOriginalBox.south();
    qreal partX = lonDist / scaleGlobal.width();
    qreal partY = latDist / scaleGlobal.height();

    bool liesWithinLocalMap = !(partX > 1 || partY > 1 || partX < 0 || partY < 0);

    QSizeF scaleLocal = originalMapScale();

    outPoint.setX(partX * scaleLocal.width() + mOrigin.x());
    outPoint.setY(partY * scaleLocal.height() + mOrigin.y());

    return liesWithinLocalMap;
}

void LocalMapLogic::localToRobot(const QPointF &point, MapAbstraction::MapRobotObjectConstPtr robot,
                                 QPointF &robotPoint) const
{
    QPointF robotLocalPosition;
    globalToLocal(MapLibraryHelpers::transformCoords(robot->coords()), robotLocalPosition);

    qreal transX = robotLocalPosition.x();
    qreal transY = robotLocalPosition.y();
    qreal alph = robot->orientation() + mRotation;
    qreal x = point.x() - transX;
    qreal y = point.y() - transY;
    qreal xRot = x*cos(alph) + y*sin(alph);
    qreal yRot = -x*sin(alph) + y*cos(alph);
    //qDebug("Point in robot frame position %f %f, |%f, %f|", xRot, yRot, x, y);

    robotPoint.setX(xRot);
    robotPoint.setY(yRot);
}

bool LocalMapLogic::hasReferenceFrame(GeoObjectID id) const
{
    return mRobotReferenceFrames.contains(id);
}

void LocalMapLogic::updateRefereceRobotFrame(GeoObjectID id, MapAbstraction::MapRobotObjectPtr robot)
{
    MapAbstraction::MapObjectPtr frozen(robot->Clone());
    QPointF local;
    globalToLocal(MapLibraryHelpers::transformCoords(robot->coords()), local);
    frozen->setCoords(MapAbstraction::GeoCoords(local.x(), local.y()));
    mRobotReferenceFrames.insert(id, frozen.staticCast<MapAbstraction::MapRobotObject>());
}

MapAbstraction::MapRobotObjectPtr LocalMapLogic::getReferenceFrame(GeoObjectID id) const
{
    Q_ASSERT(hasReferenceFrame(id));
    MapAbstraction::MapObjectPtr defrosted(mRobotReferenceFrames.value(id)->Clone());
    MapAbstraction::MapRobotObjectPtr robot = defrosted.staticCast<MapAbstraction::MapRobotObject>();

    QPointF local(robot->coords().longitude(), robot->coords().latitude());
    robot->setCoords(MapLibraryHelpers::transformCoords(geoLocalizePoint(local)));

    return robot;
}

GeoDataCoordinates LocalMapLogic::robotToGlobal(const QPointF &robotLocalPoint,
                                                MapAbstraction::MapRobotObjectConstPtr robotObject) const
{
    QPointF localCoords = robotToLocal(robotLocalPoint, robotObject);
    //qDebug("local Coords %f %f", localCoords.x(), localCoords.y());
    return geoLocalizePoint(localCoords);
}

bool LocalMapLogic::globalToRobot(const Marble::GeoDataCoordinates &globalPoint, MapRobotObjectPtr robot,
                                  QPointF &robotPoint) const
{
    QPointF localPoint;
    bool inMap = globalToLocal(globalPoint, localPoint);
    localToRobot(localPoint, robot, robotPoint);
    return inMap;
}

void LocalMapLogic::calculateRelativePosition(GeoObjectID id, MapRobotObjectPtr oldRobot,
                                              MapRobotObjectPtr newRobot, QPointF &robotLocalPoint, qreal &orientation)
{
    if (MapObject::LocalRelative == oldRobot->localizationType())
    {
        globalToRobot(MapLibraryHelpers::transformCoords(newRobot->coords()), oldRobot, robotLocalPoint);
        orientation = newRobot->orientation();
        qDebug("Sending manual placement with local relative coords");
    }
    else if (MapObject::LocalAbsolute == oldRobot->localizationType())
    {
        globalToLocal(MapLibraryHelpers::transformCoords(newRobot->coords()), robotLocalPoint);
        orientation = globalToLocalRotation(newRobot->orientation());
        qDebug("Sending manual placement with local absolute coords");
    }
    else
    {   //Cannot position global robot yet
        qWarning("Global localization");
        return;
    }
    updateRefereceRobotFrame(id, newRobot);
}

bool LocalMapLogic::localizeRobot(GeoObjectID id, MapAbstraction::MapRobotObjectPtr robotObjectData)
{
    GeoDataCoordinates newCoords;
    if (robotObjectData->localizationType() == MapObject::LocalAbsolute)
    {
        newCoords = geoLocalizePoint(QPointF(robotObjectData->coords().longitude(),
                                          robotObjectData->coords().latitude()));
        robotObjectData->setOrientation(geoLocalizeRotation(robotObjectData->orientation()));
        robotObjectData->setCoords(MapLibraryHelpers::transformCoords(newCoords));
    }
    else if (robotObjectData->localizationType() == MapObject::LocalRelative)
    {
        //qDebug("Incoming coords: %f, %f", robotObjectData->coords().latitude(), robotObjectData->coords().longitude());

        MapRobotObjectPtr referenceRobot;
        if (hasReferenceFrame(id))
        {
            referenceRobot = getReferenceFrame(id);
        }
        else
        {   //TODO - we are using the robot object as data object here. It's not too good
            GeoDataCoordinates localZeroInGlobal = geoLocalizePoint(QPointF(0,0));
            QString e;
            referenceRobot.reset(new MapRobotObject(MapLibraryHelpers::transformCoords(localZeroInGlobal),
                                                    0, RobotStateNormal, e, e, e, 0, 0));
        }

        newCoords = robotToGlobal(
                    QPointF(robotObjectData->coords().longitude(),
                    robotObjectData->coords().latitude()), referenceRobot);

        //qDebug("Coords old: |%f,%f,%f|", robotObjectData->coords().longitude(), robotObjectData->coords().latitude(), robotObjectData->orientation());

        //qDebug("orientations: %f, %f, sum %f", robotObjectData->orientation(), referenceRobot->orientation(),
        //       robotObjectData->orientation() + referenceRobot->orientation());
        robotObjectData->setOrientation(geoLocalizeRotation(robotObjectData->orientation()));
        robotObjectData->setOrientation(robotObjectData->orientation() + referenceRobot->orientation()); //TODO
    }

    robotObjectData->setCoords(MapLibraryHelpers::transformCoords(newCoords));
    //Don't: robotObjectData->setLocalizationType(MapObject::Global);
}
