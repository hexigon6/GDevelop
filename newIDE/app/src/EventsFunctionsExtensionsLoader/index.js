// @flow
import { mapVector } from '../Utils/MapFor';
import { mapFor } from '../Utils/MapFor';
import slugs from 'slugs';

const gd = global.gd;

export type EventsFunctionWriter = {|
  getIncludeFileFor: (functionName: string) => string,
  writeFunctionCode: (functionName: string, code: string) => Promise<void>,
|};

const mangleName = (name: string) => {
  return slugs(name, '_', []);
};

/**
 * Load all events functions of a project in extensions
 */
export const loadProjectEventsFunctionsExtensions = (
  project: gdProject,
  eventsFunctionWriter: EventsFunctionWriter
): Promise<void> => {
  return Promise.all(
    mapFor(0, project.getEventsFunctionsExtensionsCount(), i => {
      return loadProjectEventsFunctionsExtension(
        project,
        project.getEventsFunctionsExtensionAt(i),
        eventsFunctionWriter
      );
    })
  );
};

const loadProjectEventsFunctionsExtension = (
  project: gdProject,
  eventsFunctionsExtension: gdEventsFunctionsExtension,
  eventsFunctionWriter: EventsFunctionWriter
): Promise<void> => {
  return generateEventsFunctionExtension(
    project,
    eventsFunctionsExtension,
    eventsFunctionWriter
  ).then(extension => {
    gd.JsPlatform.get().addNewExtension(extension);
    extension.delete();
  });
};

/**
 * Generate the code for the given events functions
 */
const generateEventsFunctionExtension = (
  project: gdProject,
  eventsFunctionsExtension: gdEventsFunctionsExtension,
  eventsFunctionWriter: EventsFunctionWriter
): Promise<gdEventsFunctionsExtension> => {
  const extension = new gd.PlatformExtension();

  extension.setExtensionInformation(
    eventsFunctionsExtension.getName(),
    eventsFunctionsExtension.getFullName(),
    eventsFunctionsExtension.getDescription(),
    '',
    '' //TODO - Author and license support?
  );

  return Promise.all(
    mapVector(
      eventsFunctionsExtension.getEventsFunctions(),
      (eventsFunction: gdEventsFunction) => {
        const functionType = eventsFunction.getFunctionType();
        let instructionOrExpression;
        if (functionType === gd.EventsFunction.Expression) {
          instructionOrExpression = extension.addExpression(
            eventsFunction.getName(),
            eventsFunction.getFullName() || 'Unnamed expression',
            eventsFunction.getDescription(),
            eventsFunctionsExtension.getFullName() || 'Unnamed extension',
            'res/function.png'
          );
        } else if (functionType === gd.EventsFunction.StringExpression) {
          instructionOrExpression = extension.addStrExpression(
            eventsFunction.getName(),
            eventsFunction.getFullName() || 'Unnamed string expression',
            eventsFunction.getDescription(),
            eventsFunctionsExtension.getFullName() || 'Unnamed extension',
            'res/function.png'
          );
        } else if (functionType === gd.EventsFunction.Condition) {
          instructionOrExpression = extension.addCondition(
            eventsFunction.getName(),
            eventsFunction.getFullName() || 'Unnamed condition',
            eventsFunction.getDescription(),
            eventsFunction.getSentence(),
            eventsFunctionsExtension.getFullName() || 'Unnamed extension',
            'res/function.png',
            'res/function24.png'
          );
        } else {
          instructionOrExpression = extension.addAction(
            eventsFunction.getName(),
            eventsFunction.getFullName() || 'Unnamed action',
            eventsFunction.getDescription(),
            eventsFunction.getSentence(),
            eventsFunctionsExtension.getFullName() || 'Unnamed extension',
            'res/function.png',
            'res/function24.png'
          );
        }

        mapVector(
          eventsFunction.getParameters(),
          (parameter: gdParameterMetadata) => {
            if (!parameter.isCodeOnly()) {
              instructionOrExpression.addParameter(
                parameter.getType(),
                parameter.getDescription(),
                parameter.getExtraInfo(),
                parameter.isOptional()
              );
            } else {
              instructionOrExpression.addCodeOnlyParameter(
                parameter.getType(),
                parameter.getExtraInfo()
              );
            }
          }
        );

        const includeFiles = new gd.SetString();
        const codeNamespace =
          'gdjs.eventsFunction__' +
          mangleName(eventsFunctionsExtension.getName()) +
          '__' +
          mangleName(eventsFunction.getName());
        const functionName = codeNamespace + '.func';
        const code = gd.EventsCodeGenerator.generateEventsFunctionCode(
          project,
          eventsFunction,
          codeNamespace,
          includeFiles,
          true //TODO
        );

        const codeExtraInformation = instructionOrExpression.getCodeExtraInformation();
        codeExtraInformation
          .setIncludeFile(eventsFunctionWriter.getIncludeFileFor(functionName))
          .setFunctionName(functionName);

        // Add any include file required by the function to the list
        // of include files for this function (so that when used, the "dependencies"
        // are transitively included).
        includeFiles
          .toNewVectorString()
          .toJSArray()
          .forEach((includeFile: string) => {
            codeExtraInformation.addIncludeFile(includeFile);
          });

        includeFiles.delete();

        return eventsFunctionWriter.writeFunctionCode(functionName, code);
      }
    )
  ).then(() => extension);
};

/**
 * Unload all extensions providing events functions of a project
 */
export const unloadProjectEventsFunctionsExtensions = (
  project: gdProject
): Promise<void> => {
  return Promise.all(
    mapFor(0, project.getEventsFunctionsExtensionsCount(), i => {
      gd.JsPlatform
        .get()
        .removeExtension(project.getEventsFunctionsExtensionAt(i).getName());
    })
  );
};
